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

// Read one text entry (e.g. "Metadata/project_settings.config") from the
// packaged .gcode.3mf — the second write-back surface, written from m_config
// during packaging. Same shell-out rationale as extract_gcode.
std::string extract_3mf_text(const std::string& gcode_3mf_path, const std::string& entry)
{
    const std::string tmp = "/tmp/orca_test_hf_extract_entry.txt";
    boost::filesystem::remove(tmp);
    const std::string cmd =
        "python3 -c \"import zipfile,sys;"
        "z=zipfile.ZipFile('" + gcode_3mf_path + "');"
        "sys.exit(1) if '" + entry + "' not in z.namelist() else "
        "open('" + tmp + "','wb').write(z.read('" + entry + "'))\"";
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

    // Write-back surface 2 (P-11 acceptance "含 3mf 内嵌 config"): the packaged
    // project_settings.config is written from m_config, which the fix no
    // longer zeroes — it must carry the project value too. A packaging-side
    // regression (config source swap, re-zero before store) turns this red.
    const std::string settings =
        extract_3mf_text("/tmp/orca_test_brim_echo.gcode.3mf", "Metadata/project_settings.config");
    REQUIRE(settings.find("\"brim_width\": \"5\"") != std::string::npos);

    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_brim_echo.gcode.3mf", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract.gcode", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract_entry.txt", ec);
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
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_nvt_echo";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);

    // Pre-run cleanup: a previously FAILED run aborts before its own cleanup
    // and leaves a stale .gcode.3mf behind; generate_output_path then
    // relocates every retry's artifact to a timestamped path (multi-process
    // collision protection), so the test would re-read the stale gcode and
    // fail forever. Remove it up front so each run judges only its own
    // output.
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

TEST_CASE("Wipe tower rotation angle survives process preset substitution", "[integration][highflow]")
{
    // P-30: wipe_tower_rotation_angle is project-owned state (the desktop
    // keeps it in PresetBundle::project_config, outside preset substitution).
    // Official process JSONs never declare it — fdm_process_idex's 90 being
    // the sole, deliberate exception — so Preset::normalize backfills the
    // code default 0 into official->config and the wholesale overwrite used
    // to reset a 3MF's rotation (HueForge ships 270°; the tower then rotates
    // ~30mm toward the bed edge and 4.7mm out of the printable area).
    // Post-fix contract, mirroring the nvt case above: the header echo and
    // the packaged project_settings.config both carry the 3MF's value.
    // The fixture (u1_preset_test_wipe_tower_rot) is the single-object
    // baseline with rotation=270 + dithering_local_z_mode=1 grafted into
    // project_settings.config — it has no tower geometry, so the rotation
    // is write-back fidelity, exactly like nozzle_volume_type here.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/u1_preset_test_wipe_tower_rot.3mf";
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_wt_rot_echo";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);

    // Pre-run cleanup: same stale-artifact hazard as the nvt case — a
    // previous failed run's leftover would relocate this run's output and
    // the assertions below would read the stale file forever.
    boost::filesystem::remove("/tmp/orca_test_wt_rot_echo.gcode.3mf");

    engine.run();

    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    const std::string gcode = extract_gcode("/tmp/orca_test_wt_rot_echo.gcode.3mf");
    // Prefix match covers both echo formats ("270" / "270.000").
    REQUIRE(gcode.find("wipe_tower_rotation_angle = 270") != std::string::npos);
    // The coBool sibling from the same candidate table survives too.
    REQUIRE(gcode.find("dithering_local_z_mode = 1") != std::string::npos);

    // Write-back surface 2: the packaged project_settings.config is written
    // from m_config post-substitution — the preserved value must round-trip.
    const std::string settings =
        extract_3mf_text("/tmp/orca_test_wt_rot_echo.gcode.3mf", "Metadata/project_settings.config");
    REQUIRE(settings.find("\"wipe_tower_rotation_angle\": \"270\"") != std::string::npos);
    REQUIRE(settings.find("\"dithering_local_z_mode\": \"1\"") != std::string::npos);

    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_wt_rot_echo.gcode.3mf", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract.gcode", ec);
    boost::filesystem::remove("/tmp/orca_test_hf_extract_entry.txt", ec);
}

TEST_CASE("Output path relocation is surfaced, not silent", "[integration][slice]")
{
    // generate_output_path (Utils.cpp) appends a timestamp suffix when the
    // requested output path is already occupied — deliberate multi-process
    // collision protection, kept unchanged by design. The hazard: rc stays 0
    // and the artifact lands elsewhere, so a caller reading the requested
    // path would serve the stale leftover. Contract (observability fix,
    // 2026-09-20): when relocation happens the engine emits a task-level
    // OUTPUT_PATH_RELOCATED warning, and stats().output_path() (JSON:
    // print_info_total.output_file) reports where the artifact really went.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    const std::string contract_path = "/tmp/orca_test_relocate.gcode.3mf";

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/u1_preset_test.3mf";
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_relocate";

    // Also clears any timestamped leftovers from earlier runs of this test.
    boost::filesystem::remove(contract_path);
    for (boost::filesystem::directory_iterator it("/tmp"), end; it != end; ++it)
    {
        if (it->path().filename().string().rfind("orca_test_relocate.gcode_", 0) == 0)
            boost::filesystem::remove(it->path());
    }

    // Occupy the contract path with a stale leftover, as a previously failed
    // (or interrupted) task would.
    {
        std::ofstream f(contract_path, std::ios::binary);
        f << "STALE";
    }

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    engine.run();

    // The slice itself must succeed; relocation is not a failure.
    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    // Relocation happened: artifact exists at the reported path, which is
    // NOT the requested one; the stale bytes stay untouched at the contract
    // path (behavior unchanged by design).
    REQUIRE(engine.output_path() != contract_path);
    REQUIRE(boost::filesystem::exists(engine.output_path()));
    REQUIRE(boost::filesystem::exists(contract_path));

    // The relocation must be visible in the issue list, not just in the log.
    bool warned = false;
    for (const auto& iss : engine.stats().issues)
        if (iss.code == "OUTPUT_PATH_RELOCATED" && iss.plate_id == -1)
            warned = true;
    REQUIRE(warned);

    boost::system::error_code ec;
    boost::filesystem::remove(contract_path, ec);
    boost::filesystem::remove(engine.output_path(), ec);
}

// ============================================================================
// P-35: filament z_hop override must reach the G-code generator at runtime
// ============================================================================

// Count spiral-lift full circles (G3 with the P1 360-degree marker) per
// current tool. _spiral_travel_to_z emits "G3 ... P1" once per forced
// layer-change SpiralLift; ownership = the last T command before the arc.
static std::map<int, int> spiral_full_circles_per_tool(const std::string& gcode)
{
    std::map<int, int> counts;
    std::regex  tool_re(R"(^T(\d+))");
    std::smatch m;
    int         cur = -1;
    std::istringstream iss(gcode);
    std::string line;
    while (std::getline(iss, line))
    {
        if (std::regex_search(line, m, tool_re))
            cur = std::stoi(m[1]);
        if (line.rfind("G3 ", 0) == 0 && line.find(" P1") != std::string::npos)
            ++counts[cur];
    }
    return counts;
}

// List the packaged per-plate G-code entry names (e.g. "Metadata/plate_1.gcode").
static std::vector<std::string> gcode_entries(const std::string& gcode_3mf_path)
{
    const std::string tmp = "/tmp/orca_test_zhop_entries.txt";
    boost::filesystem::remove(tmp);
    const std::string cmd =
        "python3 -c \"import zipfile;"
        "z=zipfile.ZipFile('" + gcode_3mf_path + "');"
        "open('" + tmp + "','w').write(' '.join(n for n in z.namelist() if n.endswith('.gcode')))\"";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::ifstream f(tmp);
    REQUIRE(f.good());
    std::vector<std::string> names;
    std::string              line;
    while (std::getline(f, line, ' '))
        if (!line.empty())
            names.push_back(line);
    boost::filesystem::remove(tmp);
    return names;
}

// Assert no G2/G3 arc moves on any plate of the packaged output, with
// per-tool spiral attribution in the failure message. Shared regression
// guard for the P-35 field fixture (correct composed data) and the S2
// nil-variant fixture (stale/nil project data).
static void require_zero_arcs_all_plates(const std::string& gcode_3mf_path)
{
    for (const auto& entry : gcode_entries(gcode_3mf_path))
    {
        const std::string g = extract_3mf_text(gcode_3mf_path, entry);
        int arcs = 0;
        std::istringstream iss(g);
        std::string line;
        while (std::getline(iss, line))
            if (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
                ++arcs;
        const auto spirals = spiral_full_circles_per_tool(g);
        int         spiral_total = 0;
        for (const auto& [tool, n] : spirals)
            spiral_total += n;
        std::ostringstream per_tool;
        for (const auto& [t, n] : spirals)
            per_tool << "T" << t << ":" << n << " ";
        INFO(entry << ": arcs=" << arcs << " spirals=" << spiral_total
                   << " per-tool=" << per_tool.str());
        REQUIRE(arcs == 0);
    }
}

TEST_CASE("P-35: filament z_hop override reaches runtime, no spiral arcs (2033 field fixture)",
          "[integration][zhop]")
{
    // Fixture = the recovered 2033 cloud-task input: bed-filling 270 mm
    // walls-only cube (1350 layers) on Snapmaker U1 0.4, SnapSpeed x4. The
    // project carries the full desktop-composed flow data
    // (filament_z_hop_types = [Slope Lift x8], filament_flow_step_size =
    // [2,2,2,2], interleaved filament_flow_support) while the machine base
    // z_hop_types stays "Auto Lift" x4.
    //
    // Pre-P-35 (fork 376524d9a6 ordering bug in GCode::_do_export) the
    // engine's generator config was re-clobbered with the PRE-override
    // full-print-config snapshot, so the runtime saw flat Auto Lift ->
    // every layer change forced SpiralLift -> 1344 full-circle G3 arcs per
    // cube plate with r = 1.217 mm sweeping outside the bed (X -0.548) ->
    // U1 firmware refused the file. Post-fix the override wins: lift
    // resolves to Slope (straight ramps, line-identical to desktop 2.4.1
    // output at the failure site), zero arcs, and the near-boundary warning
    // must not fire because no slot's effective lift type stays
    // spiral-active.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    const std::string out3mf = "/tmp/orca_test_zhop_repro.gcode.3mf";
    {   // Pre-clean: an occupied output path silently relocates (P-17 trap).
        boost::system::error_code ec;
        boost::filesystem::remove(out3mf, ec);
    }

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/u1_spiral_zhop_repro.3mf";
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_zhop_repro";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    engine.run();

    // Success-path test: substitution/validation regressions surface as
    // false failures here rather than silently passing.
    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    // Effective-value warning gate: the machine base is Auto Lift x4, but
    // after the override every slot's effective lift is Slope. A
    // SPIRAL_LIFT_NEAR_BOUNDARY warning here would be the false positive
    // the P-35 check_spiral_lift_near_boundary fix removes.
    for (const auto& iss : engine.stats().issues)
    {
        INFO("issue " << iss.code << " plate " << iss.plate_id);
        REQUIRE(iss.code != "SPIRAL_LIFT_NEAR_BOUNDARY");
    }

    // Core regression: zero arcs on ANY plate (pre-fix: 1344 + 1344 + 4 + 4
    // spiral full circles). The cube is polyhedral, so no legitimate G2/G3
    // arc-fitting moves exist either.
    require_zero_arcs_all_plates(out3mf);

    // Config echo documents that the composed filament data survived the
    // pipeline. NOTE (P-35 trap): the echo comes from the PRE-override
    // full_print_config snapshot — it proves transport, not runtime; the
    // arc assertions above pin the runtime side.
    const std::string g1 = extract_3mf_text(out3mf, "Metadata/plate_1.gcode");
    REQUIRE(g1.find("; filament_z_hop_types = Slope Lift,Slope Lift,Slope Lift,Slope Lift")
            != std::string::npos);
    REQUIRE(g1.find("; filament_flow_step_size = 2,2,2,2") != std::string::npos);

    boost::system::error_code ec;
    boost::filesystem::remove(out3mf, ec);
}

TEST_CASE("S2: official flow-variant segment replaces nil project data", "[integration][zhop][nilvariant]")
{
    // Fixture derived from the 2033 field input (u1_spiral_zhop_repro.3mf):
    // identical except filament_z_hop_types is nil x8 — the "stale/nil
    // flow data" input class (old desktop exports, converter tools,
    // App-generated files). Desktop refreshes such keys from the system
    // preset on project load (PresetCollection::load_external_preset,
    // Preset.cpp:2121-2150 restores keys outside different_settings_to_
    // system); pre-S2 the engine's overwriteExtruderFrom skipped the whole
    // flow-variant family, so nil survived substitution -> the machine base
    // "Auto Lift" won at runtime -> 1344+1344 spiral full circles per run
    // (nil never engages the PrintApply override: whole-option is_nil).
    //
    // Post-S2 the official SnapSpeed segment [Slope Lift, Slope Lift] is
    // rewritten per slot before the flat copy loop: zero arcs, echo
    // collapses to Slope x4, no near-boundary false warning.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    const std::string out3mf = "/tmp/orca_test_zhop_nil.gcode.3mf";
    {   // Pre-clean: an occupied output path silently relocates (P-17 trap).
        boost::system::error_code ec;
        boost::filesystem::remove(out3mf, ec);
    }

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/u1_spiral_zhop_nil.3mf";
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_zhop_nil";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    engine.run();

    REQUIRE(engine.stats().success);
    REQUIRE(engine.exit_code() == 0);

    // Effective lift is Slope on every slot after the official rewrite.
    for (const auto& iss : engine.stats().issues)
    {
        INFO("issue " << iss.code << " plate " << iss.plate_id);
        REQUIRE(iss.code != "SPIRAL_LIFT_NEAR_BOUNDARY");
    }

    // Pre-S2: 1344 + 1344 spiral full circles (nil -> machine Auto ->
    // forced SpiralLift). Same zero-arc guard as the field fixture.
    require_zero_arcs_all_plates(out3mf);

    // The composed filament key now carries the OFFICIAL preset segment
    // (was nil passthrough pre-S2 — echo said "nil,nil,nil,nil").
    const std::string g1 = extract_3mf_text(out3mf, "Metadata/plate_1.gcode");
    REQUIRE(g1.find("; filament_z_hop_types = Slope Lift,Slope Lift,Slope Lift,Slope Lift")
            != std::string::npos);
    REQUIRE(g1.find("; filament_flow_step_size = 2,2,2,2") != std::string::npos);

    boost::system::error_code ec;
    boost::filesystem::remove(out3mf, ec);
}
