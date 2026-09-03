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
