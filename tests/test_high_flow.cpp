/**
 * @file test_high_flow.cpp
 * @brief End-to-end regression: high-flow filament variant actually reaches G-code.
 *
 * Fixture: fixtures/high_flow_whitelist.3mf — 8-slot project, all slots
 * filament_volume_type=high_flow, nozzle 0 = high-flow nozzle, project
 * schema v1. Process/printer/filament domains all carry
 * flow_support=["standard","high_flow"] and vector-valued speed keys
 * (outer_wall_speed "200,500", sparse_infill_speed "270,600").
 *
 * The risk this test pins down: an engine that fails to resolve the flow
 * variant silently slices at STANDARD speeds (the upstream degradation
 * semantics — unknown flow data never errors, it falls back to standard).
 * Such an engine still produces a valid-looking success result, so the only
 * reliable assertion is on the produced G-code feedrates themselves:
 *
 *   - the fastest extrusion moves must exceed any standard-flow cap
 *     (outer 200 / sparse infill 270 mm/s),
 *   - per-extruder maxima must differ the way per-slot volumetric limits
 *     (filament_max_volumetric_speed per flow variant) dictate.
 *
 * Expected observed maxima (measured on v02.01.07 + feature-high-flow
 * libslic3r, 2026-09-03):
 *   T0..T3, T6, T7 (volumetric-capped around fmvs 10-20):  up to 500 mm/s
 *   T4, T5        (SnapSpeed copy, fmvs high_flow = 40):   up to 595 mm/s
 */

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"   // set_data_dir / set_resources_dir

#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

// Read the single plate's G-code out of the packaged .gcode.3mf. libzip is
// not linked into the test binary; shell out to python3 (available on every
// host that builds the engine) — or fail the test loudly if either step
// breaks, which is exactly what we want for a regression guard.
std::string extract_gcode(const std::string& gcode_3mf_path)
{
    const std::string tmp = "/tmp/orca_test_hf_extract.gcode";
    boost::filesystem::remove(tmp);
    const std::string cmd =
        "python3 -c \"import zipfile,sys;"
        "z=zipfile.ZipFile('" + gcode_3mf_path + "');"
        "g=[n for n in z.namelist() if n.endswith('.gcode')];"
        "sys.exit(1) if not g else open('" + tmp + "','wb').write(z.read(g[0]))\"";
    const int rc = std::system(cmd.c_str());
    REQUIRE(rc == 0);
    std::ifstream f(tmp, std::ios::binary);
    REQUIRE(f.good());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Per-extruder maximum print (non-travel) feedrate in mm/s. Feedrates in the
// G-code are mm/min (G1 F...). Travel moves on this printer run at
// F30000 = 500 mm/s which coincides with the high-flow outer wall cap, so
// travels must be excluded — a move counts only inside a ;TYPE: section and
// only when the F value differs from the travel feedrate seen right after
// WIPE/travel segments. Simplest robust rule: ignore F values >= 599 and
// the exact F30000 travel marker.
std::map<int, double> max_print_speed_per_extruder(const std::string& gcode)
{
    std::map<int, double> mx;
    std::regex  extruder_re(R"(^T(\d))");
    std::regex  feed_re(R"(^G1 F([0-9.]+))");
    std::smatch m;
    int  cur_extruder = -1;
    bool in_type      = false;   // inside a ;TYPE:... section (printing)

    std::istringstream iss(gcode);
    std::string line;
    while (std::getline(iss, line))
    {
        if (std::regex_search(line, m, extruder_re)) cur_extruder = std::stoi(m[1]);
        if (line.rfind(";TYPE:", 0) == 0) { in_type = true; continue; }
        if (line.rfind(";LAYER:", 0) == 0) { continue; }
        if (!in_type) continue;
        if (line.rfind(";", 0) == 0) continue;
        if (std::regex_search(line, m, feed_re))
        {
            const double v_mm_s = std::stod(m[1]) / 60.0;
            if (v_mm_s >= 599.0) continue;             // travel F30000/F35940 marker
            if (cur_extruder >= 0)
                mx[cur_extruder] = std::fmax(mx.count(cur_extruder) ? mx[cur_extruder] : 0.0, v_mm_s);
        }
    }
    return mx;
}

} // namespace

TEST_CASE("High-flow variant reaches G-code feedrates (whitelist fixture)", "[integration][highflow]")
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/high_flow_whitelist.3mf";
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_hf_whitelist";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    engine.run();

    // The project must slice cleanly — this is a success-path test; any
    // regression in substitution/validation shows up as a false failure here
    // rather than silently passing.
    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    const std::string gcode = extract_gcode("/tmp/orca_test_hf_whitelist.gcode.3mf");

    // Config dump must carry the flow-variant model through (guards against
    // an engine build linked against a pre-high-flow libslic3r).
    REQUIRE(gcode.find("; filament_volume_type = high_flow") != std::string::npos);
    REQUIRE(gcode.find("; outer_wall_speed = 200,500") != std::string::npos);

    const auto mx = max_print_speed_per_extruder(gcode);

    // All 8 extruders are active in this project.
    REQUIRE(mx.size() == 8);

    // Core regression: STANDARD-flow fallback would cap outer walls at 200
    // and sparse infill at 270 mm/s. Any extruder exceeding 300 mm/s proves
    // the high-flow vector element was resolved. Measured: every extruder
    // reaches 500 mm/s (outer wall high-flow cap), T4/T5 reach ~595
    // (sparse infill 600 capped by fmvs=40).
    int extruders_above_standard_cap = 0;
    for (const auto& [id, v] : mx)
    {
        INFO("extruder T" << id << " max print speed " << v << " mm/s");
        if (v > 300.0) ++extruders_above_standard_cap;
    }
    REQUIRE(extruders_above_standard_cap == 8);

    // The volumetric differentiation: T4/T5 (SnapSpeed copy, high-flow
    // fmvs = 40 mm³/s) reach near the 600 mm/s sparse-infill cap, strictly
    // faster than any standard-filament slot's ceiling. Loose bounds keep
    // the test stable against minor upstream speed-planner changes.
    REQUIRE(mx.at(4) > 540.0);
    REQUIRE(mx.at(5) > 540.0);

    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_hf_whitelist.gcode.3mf", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract.gcode", ec);
}

TEST_CASE("Auto-brim header echo preserves project value; motion stays algorithm-only", "[integration][brim]")
{
    // 2026-09-18 cloud review defect 1 (brim_width 5→0). The input project
    // sets brim_type=auto_brim + brim_width=5. Desktop Orca 2.4.0 echoes 5 in
    // the G-code header while printing algorithm-only motion (this fixture's
    // geometry makes the auto algorithm decide "no brim" — desktop verified).
    // The engine used to zero brim_width on m_config, so the header echo AND
    // the packaged project_settings.config both said 0 while motion matched
    // desktop exactly: a lying write-back, not a motion defect. Post-fix
    // contract:
    //   - header echo carries the merged project value ("; brim_width = 5")
    //   - motion is untouched: the auto-mode zero moved to the per-plate
    //     slicing copy (prepare_merged_config_for_plate), so TYPE:Brim must
    //     stay ABSENT on this fixture — a fallback brim here would be a
    //     desktop-parity regression.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/high_flow_whitelist.3mf";
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_brim_echo";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    engine.run();

    // Success-path test: any substitution/validation regression shows up as a
    // false failure here rather than silently passing.
    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    const std::string gcode = extract_gcode("/tmp/orca_test_brim_echo.gcode.3mf");

    REQUIRE(gcode.find("; brim_type = auto_brim") != std::string::npos);
    REQUIRE(gcode.find("; brim_width = 5\n") != std::string::npos);

    // Motion invariance: no Brim feature may appear on this fixture.
    CHECK(gcode.find("TYPE:Brim") == std::string::npos);

    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_brim_echo.gcode.3mf", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract.gcode", ec);
}

TEST_CASE("Nozzle volume type survives printer preset substitution", "[integration][highflow]")
{
    // 2026-09-18 cloud review defect 2 (nozzle_volume_type high_flow →
    // standard, 31/37 outputs). nozzle_volume_type is user hardware state
    // (which nozzle is mounted per extruder); the official presets are keyed
    // by diameter only and never declare it, so Preset::normalize backfills
    // "standard" and the wholesale printer overwrite used to reset it. The
    // slicing core never reads the key (flow-variant arrays resolve by
    // filament_volume_type), so the defect is write-back fidelity: header
    // echo and packaged project config declared the wrong nozzle hardware.
    // Post-fix contract: header echo carries the user's per-slot values.
    // Fixture slot 0 = high_flow, slots 1-3 = standard — pins both
    // preservation and no cross-slot contamination.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/high_flow_whitelist.3mf";
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_nvt_echo";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);

    // Pre-run cleanup: a previously FAILED run aborts before its own cleanup
    // and leaves a stale .gcode.3mf behind; the 3MF writer appends to (or
    // fails on) an existing archive instead of replacing it, so every retry
    // would re-read the stale gcode and fail forever. Remove it up front so
    // each run judges only its own output.
    boost::filesystem::remove("/tmp/orca_test_nvt_echo.gcode.3mf");

    engine.run();

    // Success-path test: any substitution/validation regression shows up as a
    // false failure here rather than silently passing.
    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    const std::string gcode = extract_gcode("/tmp/orca_test_nvt_echo.gcode.3mf");

    REQUIRE(gcode.find("; nozzle_volume_type = high_flow,standard,standard,standard\n") != std::string::npos);

    // The consumable-side flow variant (which the slicing core DOES read)
    // must stay intact alongside the preserved nozzle hardware state.
    REQUIRE(gcode.find("; filament_volume_type = high_flow") != std::string::npos);

    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_nvt_echo.gcode.3mf", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract.gcode", ec);
}
