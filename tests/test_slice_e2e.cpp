/**
 * @file test_slice_e2e.cpp
 * @brief End-to-end integration test driving the FULL run() pipeline
 * (geometry + per-plate slicing + packaging), asserting slicing outcomes
 * (success / structured failure) across real fixtures.
 *
 * Companion to test_geometry_preprocess.cpp (which stops before slicing).
 * Tag: [integration][slice].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"         // set_data_dir / set_resources_dir

#include "Types.hpp"                   // SliceOutputStats, Issue, IssueLevel
#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

// RAII: remove a path on scope exit. Cleans the gcode.3mf that run() produces.
struct PathCleanup {
    std::string path;
    explicit PathCleanup(std::string p) : path(std::move(p))
    {
        // Pre-clean: a leftover artifact from a killed previous run (SIGTERM
        // mid-case never reaches the destructor) must not satisfy a later
        // run's exists()/file_size() assertions — stale-artifact false green.
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    }
    ~PathCleanup() {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    }
};

// Drive the full pipeline on a fixture, writing output to /tmp (output_base)
// so the fixtures directory is not polluted. Returns the engine by value-via-
// unique_ptr so callers can inspect stats().
struct SliceRun {
    std::unique_ptr<SliceEngine> engine;
    std::string output_file;        // expected gcode.3mf path under /tmp
    std::vector<std::string> temp_files;
    PathCleanup cleanup;
    SliceRun() : cleanup("") {}
};

std::unique_ptr<SliceRun> run_full(const char* fixture_path, const char* tag)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    auto r = std::make_unique<SliceRun>();
    std::string base = std::string("/tmp/orca_test_") + tag;
    r->output_file = base + ".gcode.3mf";
    r->cleanup = PathCleanup(r->output_file);

    EngineConfig cfg;
    cfg.input_file = fixture_path;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = base;
    r->engine = std::make_unique<SliceEngine>(cfg, r->temp_files);
    r->engine->run();   // run; callers assert the outcome
    return r;
}

// Variant of run_full that overrides the output format / single-plate / plate-id
// (the 2-arg overload above hardcodes format=GCODE_3MF, single_plate=false).
// The output_file extension mirrors generate_output_path() in Utils.cpp so
// PathCleanup removes the file run() actually produced: single-plate GCODE
// writes a ".gcode" file directly (SliceEngine.cpp:2733); every other combo
// packages a ".gcode.3mf". Getting this wrong leaks a temp file and, via the
// collision-suffix logic in Utils.cpp, mangles the path on the next run.
std::unique_ptr<SliceRun> run_full(const char* fixture_path, const char* tag,
                                   OutputFormat format, bool single_plate, int plate_id)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    auto r = std::make_unique<SliceRun>();
    std::string base = std::string("/tmp/orca_test_") + tag;
    const bool single_gcode = (single_plate && format == OutputFormat::GCODE);
    r->output_file = base + (single_gcode ? ".gcode" : ".gcode.3mf");
    r->cleanup = PathCleanup(r->output_file);

    EngineConfig cfg;
    cfg.input_file = fixture_path;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = base;
    cfg.format = format;
    cfg.single_plate = single_plate;
    cfg.plate_id = plate_id;
    r->engine = std::make_unique<SliceEngine>(cfg, r->temp_files);
    r->engine->run();
    return r;
}

// Does the global issues list contain a given code at a given level?
bool has_global_issue(const SliceOutputStats& s, const std::string& code, IssueLevel level)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == level)
            return true;
    return false;
}

// Same, but scoped to one plate (Issue::plate_id holds the internal 0-based
// plate index; the JSON layer converts to 1-based for display).
bool has_plate_issue(const SliceOutputStats& s, int plate_id, const std::string& code, IssueLevel level)
{
    for (const auto& iss : s.issues)
        if (iss.plate_id == plate_id && iss.code == code && iss.level == level)
            return true;
    return false;
}

} // namespace

// ============================================================================
// Case 1 — A clean model slices end-to-end: run() succeeds, a non-empty
// gcode.3mf is packaged, and the plate carries real filament/time stats.
// Verifies the full pipeline (load → preset sub → geometry → slice → export →
// package) actually produces printable output.
// ============================================================================
TEST_CASE("Clean model slices end-to-end and produces gcode.3mf", "[integration][slice][ok]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/geom_above_bed_ok.3mf", "above_bed_ok");

    REQUIRE(r->engine->stats().success);
    REQUIRE_FALSE(r->engine->stats().plates.empty());

    const auto& plate = r->engine->stats().plates.front();
    REQUIRE(plate.success);
    REQUIRE(plate.gcode_file == r->output_file);

    // The packaged gcode.3mf must exist and be non-trivially sized.
    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);

    // Real slicing produced real filament usage and print time (sign invariants
    // only — exact values depend on the model/profile).
    REQUIRE(plate.total_filament_g > 0.0);
    REQUIRE(plate.print_time > 0.0f);

    REQUIRE(!r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 0);
}

// ============================================================================
// Case 2 — A model that exceeds the bed footprint is rejected at the
// build-volume stage, NOT sliced. run() fails with exit 6; the plate is
// present but unsuccessful with an empty gcode; a BUILD_VOLUME_OUTSIDE_XY
// error issue is emitted.
// ============================================================================
TEST_CASE("Out-of-bed model fails at build-volume check", "[integration][slice][fail]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_oob_fail.3mf", "oob_fail");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);

    // The plate exists but did not produce output.
    REQUIRE_FALSE(r->engine->stats().plates.empty());
    const auto& plate = r->engine->stats().plates.front();
    REQUIRE_FALSE(plate.success);
    REQUIRE(plate.gcode_file.empty());

    // No gcode.3mf should have been packaged.
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    // The failure must be classified as a build-volume XY error.
    REQUIRE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_OUTSIDE_XY", IssueLevel::error));
}

// ============================================================================
// Case 3 — A too-tall model is rejected at the build-volume stage with a
// BUILD_VOLUME_TOO_HIGH error. (Same structure as Case 2, different cause.)
// ============================================================================
TEST_CASE("Too-tall model fails at build-volume check", "[integration][slice][fail]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/geom_too_high.3mf", "too_high");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    REQUIRE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_TOO_HIGH", IssueLevel::error));
}

// ============================================================================
// Case 4 — A partially-sunk model slices successfully (the below-bed portion
// is clipped, not printed), AND emits an OBJECT_INTENTIONALLY_BELOW_BED
// warning so the user knows part of the model is below z=0.
// ============================================================================
TEST_CASE("Partial-sink model slices with a below-bed warning", "[integration][slice][ok]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/geom_partial_sink_ok.3mf", "partial_sink_ok");

    REQUIRE(r->engine->stats().success);
    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(r->engine->exit_code() == 0);

    // The below-bed notice must be surfaced as a warning.
    REQUIRE(has_global_issue(r->engine->stats(), "OBJECT_INTENTIONALLY_BELOW_BED", IssueLevel::warning));
}

// ============================================================================
// Case 4b — A fully-below-bed model (27mm cube, world Z -53.5..-26.5, nothing
// above z=0) is auto-raised by ensure_on_bed and slices successfully, with an
// OBJECT_BELOW_BED_ADJUSTED warning. Complements Case 4: the partial sink is
// preserved (INTENTIONALLY_BELOW_BED), the full sink must be lifted.
// ============================================================================
TEST_CASE("Fully-below-bed model is auto-raised with an adjusted warning", "[integration][slice][ok]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/geom_fully_below_bed.3mf", "fully_below_bed");

    REQUIRE(r->engine->stats().success);
    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(r->engine->exit_code() == 0);

    // The lift must be surfaced as a warning (SliceEngine.cpp:1782).
    REQUIRE(has_global_issue(r->engine->stats(), "OBJECT_BELOW_BED_ADJUSTED", IssueLevel::warning));
    // It was lifted, not left intentionally sunk.
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "OBJECT_INTENTIONALLY_BELOW_BED", IssueLevel::warning));
}

// ============================================================================
// Case 5 — Single-plate GCODE mode (run() lines 235-261). With single_plate=
// true + format=GCODE, run() processes exactly one plate (plates_to_process =
// [plate_id-1]), writes the G-code directly to m_output_path (a ".gcode" file,
// NOT a ".gcode.3mf" package), and the package_output() gate at line 253
// (which requires format==GCODE_3MF for single_plate) never fires. Verifies
// the single-plate output path end-to-end, complementing the pure-function
// generate_output_path unit tests in test_utils.cpp.
// ============================================================================
TEST_CASE("Single-plate GCODE mode writes one .gcode, no .gcode.3mf", "[integration][slice][ok][single_plate]")
{
    // distinct_per_plate.3mf has 4 plates; requesting plate 1 (internal 0)
    // genuinely differs from all-plates mode. Plate 0 is a clean single-extruder
    // slice (see test_filament_count.cpp "distinct filament per plate").
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/filement_count/distinct_per_plate.3mf",
                      "single_plate_gcode", OutputFormat::GCODE, /*single_plate=*/true, /*plate_id=*/1);

    REQUIRE(r->engine->stats().success);
    // Only the requested plate was processed.
    REQUIRE(r->engine->stats().plates.size() == 1);
    const auto& plate = r->engine->stats().plates.front();
    REQUIRE(plate.success);
    // gcode_file mirrors m_output_path (assemble_plate_stats, SliceEngine.cpp:3378),
    // which is the .gcode path, not a .gcode.3mf path.
    REQUIRE(plate.gcode_file == "/tmp/orca_test_single_plate_gcode.gcode");

    // Real slicing produced real filament/time (sign invariants only).
    REQUIRE(plate.total_filament_g > 0.0);
    REQUIRE(plate.print_time > 0.0f);

    REQUIRE(!r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 0);

    // The .gcode file is written directly by export_gcode (SliceEngine.cpp:2733).
    const std::string gcode_path = "/tmp/orca_test_single_plate_gcode.gcode";
    REQUIRE(boost::filesystem::exists(gcode_path));
    REQUIRE(boost::filesystem::file_size(gcode_path) > 1000);

    // No .gcode.3mf package: format != GCODE_3MF, so the package_output() gate
    // at SliceEngine.cpp:253 does not fire for single_plate mode.
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_test_single_plate_gcode.gcode.3mf"));

    // m_output_path was assigned inside the validate_input() block.
    REQUIRE(r->engine->output_path() == gcode_path);
}

// ============================================================================
// Case 6 — PLATE_NOT_FOUND early failure (run() lines 185-186 → validate_input
// at 1490-1523). Requesting a plate_id beyond the fixture's plate count fails
// BEFORE the slicing try-block: run() returns false at line 186, m_output_path
// is never assigned (it lives at line 230, inside the try), and the
// PLATE_NOT_FOUND issue is global (plate_id=-1) so patch_orphan_plate_issues
// skips it (SliceEngine.cpp:3540 < 0 guard) — stats.plates stays EMPTY.
// ============================================================================
TEST_CASE("Out-of-range plate_id fails with PLATE_NOT_FOUND before slicing", "[integration][slice][fail][validation]")
{
    // 4-plate fixture; request plate 99 (well beyond range).
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/filement_count/distinct_per_plate.3mf",
                      "plate_not_found", OutputFormat::GCODE, /*single_plate=*/true, /*plate_id=*/99);

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);  // EXIT_PREPROCESS_ERROR (Types.hpp:16)

    // No plate was processed — the issue is global (plate_id=-1), so no
    // placeholder plate is created. Indexing stats.plates would be UB.
    REQUIRE(r->engine->stats().plates.empty());

    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_NOT_FOUND", IssueLevel::error));

    // error_message embeds the plate number; match by substring, not exact equal.
    REQUIRE(r->engine->stats().error_message.find("not found") != std::string::npos);

    // Slicing never ran → no output of either kind.
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_test_plate_not_found.gcode"));
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_test_plate_not_found.gcode.3mf"));

    // m_output_path is assigned at SliceEngine.cpp:230, inside the try-block
    // that validate_input()'s failure skips — so output_path() is still empty.
    REQUIRE(r->engine->output_path().empty());
}

// ============================================================================
// Case 7 — All-or-nothing packaging suppression (run() lines 252-254). When a
// plate fails (m_any_error=true), package_output() is NOT called even though a
// placeholder plate exists in stats. This is distinct from Case 2, which
// asserts the failure CLASSIFICATION (BUILD_VOLUME_OUTSIDE_XY); Case 7 asserts
// the packaging GATE: the !m_any_error clause at SliceEngine.cpp:253 blocks
// output despite stats.plates being non-empty (the build-volume issue carries a
// real plate_id, so patch_orphan_plate_issues creates a placeholder failed
// plate — contrast Case 6 where plates is empty).
//
// Known secondary gap (no fixture today): the empty-gcode-layers path at
// SliceEngine.cpp:1914-1920 leaves m_plate_results genuinely non-empty on
// failure, which would exercise has_output=true && m_any_error=true directly.
// No existing fixture triggers it; this case covers the gate indirectly via
// the placeholder-plate + no-output-file combination.
// ============================================================================
TEST_CASE("Failed plate suppresses gcode.3mf packaging (all-or-nothing)", "[integration][slice][fail][package]")
{
    // slice_oob_fail.3mf: single plate, build-volume XY failure.
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_oob_fail.3mf", "oob_no_package");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);

    // A placeholder failed plate exists (build-volume issue has a real plate_id,
    // so patch_orphan_plate_issues creates one). This is the load-bearing
    // contrast with Case 6: plates is NON-empty here.
    REQUIRE_FALSE(r->engine->stats().plates.empty());
    const auto& plate = r->engine->stats().plates.front();
    REQUIRE_FALSE(plate.success);
    REQUIRE(plate.gcode_file.empty());  // assemble_plate_stats returns early on failure

    // Core gate assertion: m_any_error=true suppresses package_output() even
    // though stats.plates is non-empty. No gcode.3mf is produced.
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_test_oob_no_package.gcode.3mf"));
}

// (Case 8 removed 2026-08-25: depended on a ~30MB fixture in the external
// pool that no longer exists on this machine, so it could only ever SKIP.
// The TOOLPATH_OUTSIDE post-slice path stays covered by the wipe-tower
// single-plate case and the 2-plate mixed-outcome case, both in-repo.)

// ============================================================================
// Case 9 — wipe-tower fixture, post-2.4.1-base reality (2026-10-12).
// Small single-plate fixture (擦除塔gcode路径超限.3mf, renamed
// wipe_tower_toolpath_outside.3mf). Historically the object sat inside the
// build volume while its wipe tower + brim extruded past the bed edge
// (exit 7, TOOLPATH_OUTSIDE, package suppressed). Upstream b626b4be62
// ("Feature sync gitlab to github main #937", merged 2026-10-10) reworked
// the wipe-tower partition algorithm and GCode transition generation; the
// tower footprint for this geometry now stays inside the bed, so the
// out-of-bed condition is no longer reproducible with this in-repo fixture
// (verified live: exit 0, clean plate, package written). The case now pins
// that clean outcome — a regression that REINTRODUCES out-of-bed toolpaths
// for this fixture would flip it red. The TOOLPATH_OUTSIDE detection port
// itself stays covered by Case 11 (toolpath_outside_mixed_7855).
// ============================================================================
TEST_CASE("Wipe tower fixture stays in-bed on 2.4.1 base (Case 9 fixture drift)", "[integration][slice][ok][wipe]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/wipe_tower_toolpath_outside.3mf", "wipe_tower_inbed");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(boost::filesystem::exists(r->output_file));

    // Single plate: clean success, no out-of-bed flag anywhere.
    REQUIRE(r->engine->stats().plates.size() == 1);
    const auto& p = r->engine->stats().plates[0];
    REQUIRE(p.success);
    REQUIRE_FALSE(p.toolpath_outside);
    bool has_issue = false;
    for (const auto& iss : p.issues)
        if (iss.code == "TOOLPATH_OUTSIDE" && iss.level == IssueLevel::error)
            has_issue = true;
    REQUIRE_FALSE(has_issue);
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "TOOLPATH_OUTSIDE", IssueLevel::error));
}

// (Case 10 removed 2026-08-25: the 5479 ToolOrdering heap-corruption
// known-crash placeholder. It expected a hard SIGSEGV (upstream libslic3r
// bug, desktop crashes too — see memory [[toolordering-heap-corruption-5479]]
// for evidence and the two falsified fixes). Running it kills the whole test
// process, and ctest's unfiltered invocation matched its hidden tag, turning
// every full-suite run red. When an ASan build identifies the real heap
// corruptor (or upstream fixes it), re-add as a normal slice-success case.)

// ============================================================================
// Case 11 — mixed-outcome TOOLPATH_OUTSIDE on a 2-plate project (7855).
// In-repo, always-runnable counterpart of Case 8 (which needs a ~30MB
// external fixture and SKIPs on machines without it): plate 1's toolpaths
// exceed the bed, plate 2 stays clean. Guards the plate-local coordinate
// shift (global grid offsets must not flag every plate) at minimal cost.
// ============================================================================
TEST_CASE("Toolpath outside on 2-plate project: only offending plate flagged", "[integration][slice][fail][toolpath]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/toolpath_outside_mixed_7855.3mf", "outside_mixed");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 7);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    REQUIRE(r->engine->stats().plates.size() == 2);
    int flagged = 0;
    for (const auto& p : r->engine->stats().plates) {
        bool has_outside = false;
        for (const auto& iss : p.issues)
            if (iss.code == "TOOLPATH_OUTSIDE" && iss.level == IssueLevel::error)
                has_outside = true;
        REQUIRE(p.toolpath_outside == has_outside);
        REQUIRE(p.success == !has_outside);
        if (has_outside) ++flagged;
    }
    REQUIRE(flagged == 1);
}

// ============================================================================
// Case 12 — missing-PrintConfig-key model (10632 / historical 44ae crash).
// The 3MF omits keys (seam_slope_type, thumbnails, ...) that libslic3r
// dereferences without null checks; without the FullPrintConfig::defaults()
// backfill in normalize_loaded_config (6808fe9) loading dies with SIGSEGV.
// This fixture is the actual historical crasher (model id 44ae). Reaching
// these assertions proves the backfill keeps the pipeline alive; the clean
// single-plate success proves it does not corrupt valid config either.
// ============================================================================
TEST_CASE("Model with missing config keys slices via backfill (historical 44ae SIGSEGV)", "[integration][slice][ok][backfill]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/missing_keys_backfill_10632.3mf", "backfill_10632");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(r->engine->stats().plates.size() == 1);
    REQUIRE(boost::filesystem::exists(r->output_file));
}

// ============================================================================
// Case 12.5 — four-plate mix (圆柱体), post-2.4.1-base reality (2026-10-12).
// 64KB in-repo fixture. Historically the densest per-plate outcome mix:
//   plate 1 TOOLPATH_OUTSIDE + BED_FILAMENT_MISMATCH, plate 3 build-volume XY,
//   plates 2 & 4 clean — three detection stages interleaved, exit 7.
// Upstream b626b4be62 (merged 2026-10-10) reworked wipe-tower partitioning /
// GCode transition generation, so plate 1's out-of-bed toolpath condition no
// longer reproduces (verified live: plate 1 now clean). Remaining mix:
//   plate 3 build-volume XY (preprocess) fails, plates 1/2/4 clean, exit 6
// (EXIT_PREPROCESS_ERROR — no postprocess failure class remains). Still
// guards: per-plate independent sentencing, clean-sibling isolation, and
// exit-code class aggregation. The postprocess-in-mix coverage moved to
// Case 11 (toolpath_outside_mixed_7855).
// ============================================================================
TEST_CASE("Four-plate project: build-volume failure on one plate, clean siblings unaffected (Case 12.5 drift)", "[integration][slice][fail][mixed]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/cylinder_four_plate_mixed.3mf", "cyl_mixed");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 6);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    REQUIRE(r->engine->stats().plates.size() == 4);
    // Plate 1: was TOOLPATH_OUTSIDE + BED_FILAMENT_MISMATCH pre-2.4.1; now a
    // clean plate (fixture-drift pin — flip red if out-of-bed ever returns).
    REQUIRE(r->engine->stats().plates[0].success);
    REQUIRE_FALSE(r->engine->stats().plates[0].toolpath_outside);
    // Plate 2: clean sibling must succeed despite neighbours failing.
    REQUIRE(r->engine->stats().plates[1].success);
    // Plate 3: build-volume XY rejection (preprocessing).
    REQUIRE_FALSE(r->engine->stats().plates[2].success);
    bool has_bv = false;
    for (const auto& iss : r->engine->stats().plates[2].issues)
        if (iss.code == "BUILD_VOLUME_OUTSIDE_XY" && iss.level == IssueLevel::error)
            has_bv = true;
    REQUIRE(has_bv);
    // Plate 4: clean sibling.
    REQUIRE(r->engine->stats().plates[3].success);
}

// ============================================================================
// Cases 14 — build-volume directional subdivision fixtures (方向细分系列).
// Seven 20KB-class fixtures, one per boundary direction. Each drives the
// preprocess build-volume check into a different directional error so the
// subdivision logic (which bed edge was crossed, X vs Y vs Z) is asserted
// individually rather than "some OOB happened". Measured against the 270mm
// U1 bed; the 左右都出 case also guards the desktop-parity escalation of
// Fully_Outside from warning to blocking error (was: warning + late
// TOOLPATH_OUTSIDE at exit 7; now: BUILD_VOLUME_FULLY_OUTSIDE at exit 6,
// matching the desktop "move it fully inside or off the plate" dialog).
// ============================================================================

TEST_CASE("Build-volume directional errors: each boundary direction produces its own message", "[integration][slice][fail][buildvol]")
{
    struct Dir { const char* fixture; const char* needle; };
    const Dir cases[] = {
        {"上侧超出",    "Y=276.6 > 271.0"},
        {"下侧超出",    "Y=-20.8 < 1.0"},
        {"右侧超出",    "X=284.2 > 270.5"},
        {"左侧超出",    "X=-10.3 < 0.5"},
        {"超高模型",    "Z=297.0"},
    };
    for (const auto& c : cases)
    {
        INFO("fixture: " << c.fixture);
        auto r = run_full((std::string(ORCA_TEST_FIXTURE_DIR) + "/build_volume_dir/" + c.fixture + ".3mf").c_str(), "bv_dir");
        REQUIRE_FALSE(r->engine->stats().success);
        REQUIRE(r->engine->exit_code() == 6);
        bool found = false;
        for (const auto& iss : r->engine->stats().issues)
            if (iss.code == "BUILD_VOLUME_OUTSIDE_XY" || iss.code == "BUILD_VOLUME_TOO_HIGH")
                if (iss.message.find(c.needle) != std::string::npos)
                    found = true;
        REQUIRE(found);
    }
}

TEST_CASE("Fully-outside instance blocks at preprocess with dedicated error (左右都出)", "[integration][slice][fail][buildvol]")
{
    // Object wider than the bed: completely outside the build volume while
    // still placed on the plate. Desktop blocks this at validation; the engine
    // previously emitted only a warning and failed later at post-processing.
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/build_volume_dir/左右都出.3mf", "bv_fully");
    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 6);
    bool found = false;
    for (const auto& iss : r->engine->stats().issues)
        if (iss.code == "BUILD_VOLUME_FULLY_OUTSIDE" && iss.level == IssueLevel::error)
            found = true;
    REQUIRE(found);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));
}

TEST_CASE("Multi-direction overflow lists every crossed edge (上下左右侧超出)", "[integration][slice][fail][buildvol]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/build_volume_dir/上下左右侧超出.3mf", "bv_multi");
    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 6);
    int xy_errors = 0;
    for (const auto& iss : r->engine->stats().issues)
        if (iss.code == "BUILD_VOLUME_OUTSIDE_XY" && iss.level == IssueLevel::error)
            ++xy_errors;
    // Crosses Y-high, Y-low and X-high: at least three directional errors.
    REQUIRE(xy_errors >= 3);
}

// ============================================================================
// Case 12 — Fully-outside-XY model: a lying cylinder (279mm across, rotated
// ~5.7° around X) whose convex-hull corners are all >135mm from the bed
// center of the 270mm circular bed. calc_print_volume_state therefore
// classifies it Fully_Outside (vertex-inside test only; edge-bed
// intersection is Z-clip only). Regression: this state used to emit only a
// non-blocking warning and slicing proceeded; it must now be a blocking
// pre-process error (exit 6, no slicing stage, no output).
// ============================================================================
TEST_CASE("Fully-outside-XY model blocked at pre-process (exit 6)", "[integration][slice][fail][build_volume]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/build_volume/fully_outside_xy.3mf", "fully_outside_xy");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    REQUIRE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_FULLY_OUTSIDE", IssueLevel::error));
}

// ============================================================================
// Case 13 — Too-high AND fully-outside-XY model. A vertical 279x279
// footprint, z=271 cylinder: every convex-hull corner outside the circular
// bed (Fully_Outside) AND top z exceeds printable height 270.05.
//
// The pre-check (run_build_volume_check) fires FIRST and aborts the plate
// before Print::validate ever runs, so the height violation is carried by
// the Fully_Outside error, not by BUILD_VOLUME_TOO_HIGH — that code only
// appears when an instance is Partly_Outside (pre-check classifier) or when
// the pre-check passes but validate still catches the height (validate
// path, e.g. brim/support pushing effective layers past printable_height).
//
// Regressions locked in:
//   1. Fully_Outside blocks at pre-process with exit 6 (it used to be a
//      non-blocking warning; the plate then sliced all the way through and
//      failed only post-slice via TOOLPATH_OUTSIDE/TOOL_HEIGHT_OUTSIDE).
//   2. The abort happens BEFORE slicing, so no post-processing toolpath
//      issues may appear.
// ============================================================================
TEST_CASE("Too-high + fully-outside model: double error, aborted before slicing", "[integration][slice][fail][build_volume]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/build_volume/too_high_and_fully_outside.3mf", "too_high_fully_outside");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    // Blocking error present; BUILD_VOLUME_TOO_HIGH must NOT appear (the
    // pre-check aborts before Print::validate — see the case comment).
    REQUIRE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_FULLY_OUTSIDE", IssueLevel::error));
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_TOO_HIGH", IssueLevel::error));

    // Aborted before slicing: no post-processing toolpath diagnostics.
    for (const auto& iss : r->engine->stats().issues) {
        REQUIRE_FALSE(iss.code == "TOOLPATH_OUTSIDE");
        REQUIRE_FALSE(iss.code == "TOOL_HEIGHT_OUTSIDE");
    }
}

// ============================================================================
// Case 14 — Straddling object with almost nothing above the bed. The stored
// transform places the 27mm cube at world Z [-26.95, +0.05]: min_z < 0 AND
// max_z >= 0, so ensure_on_bed(allow_negative_z=true) semantics treat it as
// author-intended sinking and preserve it as stored (no raise). Only 0.05mm
// pokes above the bed, so slicing yields zero layers.
//
// Verified to match desktop Orca behaviour (same straddle branch, same
// "No layers were detected" failure). Regressions locked in:
//   1. The straddle is reported as OBJECT_INTENTIONALLY_BELOW_BED warning
//      (NOT OBJECT_BELOW_BED_ADJUSTED — no raise must ever fire here).
//   2. Zero layers aborts with exit 4 / SLICING_FATAL_ERROR instead of
//      producing an empty-but-"successful" gcode.
// ============================================================================
TEST_CASE("Straddling object with 0.05mm above bed: preserved, empty layers, exit 4", "[integration][slice][fail][below_bed]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/geom_straddle_empty_layers.3mf", "straddle_empty_layers");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 4);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    REQUIRE(has_global_issue(r->engine->stats(), "SLICING_FATAL_ERROR", IssueLevel::error));
    REQUIRE(has_global_issue(r->engine->stats(), "OBJECT_INTENTIONALLY_BELOW_BED", IssueLevel::warning));
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "OBJECT_BELOW_BED_ADJUSTED", IssueLevel::warning));
}

// ============================================================================
// Case 15 — Multi-plate with one empty plate: benign skip, partial success
// (slice_empty_plate_skip.3mf: plate 1 carries the cube, plate 2's <plate>
// block has no model_instance). Desktop-parity semantics: an empty plate
// among good ones is NOT a failure — the good plate slices and packages,
// rc stays 0, and the skip is visible as a warning-level EMPTY_PLATE issue
// carrying the skipped plate's id. This case would fail pre-fix in the
// observability direction (empty plate invisible in JSON) — the companion
// zero-product failures are Cases 16/17.
// ============================================================================
TEST_CASE("Multi-plate with one empty plate: benign skip, partial success", "[integration][slice][ok][empty_plate]")
{
    // Pre-clean (stale-artifact trap): generate_output_path silently relocates
    // to a timestamped path when the target exists, which would make the
    // exists() assertions below pass vacuously against a stale file.
    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_empty_plate_skip.gcode.3mf", ec);

    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_empty_plate_skip.3mf", "empty_plate_skip");

    // Partial success: good plate sliced and packaged; empty plate is a
    // benign skip (desktop parity), not a failure.
    REQUIRE(r->engine->stats().success);
    REQUIRE_FALSE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(boost::filesystem::exists(r->output_file));

    // The skip is visible as a warning attributed to the second plate
    // (internal 0-based id 1), but gets NO placeholder plate entry —
    // plates[] holds only the sliced plate, so overall success is untouched
    // (patch_orphan_plate_issues fabricates placeholders for error-level
    // orphans only).
    REQUIRE(has_plate_issue(r->engine->stats(), 1, "EMPTY_PLATE", IssueLevel::warning));
    REQUIRE(r->engine->stats().plates.size() == 1);
    REQUIRE(r->engine->stats().plates.front().plate_id == 0);
    REQUIRE(r->engine->stats().plates.front().success);

    // P-24 quiet path: this fixture's explicit map agrees with geometry, so
    // the always-rebuild reassignment must be a no-op and stay SILENT —
    // desktop-authored files never see a PLATE_MEMBERSHIP_REBUILT tip.
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "PLATE_MEMBERSHIP_REBUILT", IssueLevel::tip));
}

// ============================================================================
// Case 16 — Single-plate mode requesting the EMPTY plate (plate_id=2 of
// slice_empty_plate_skip.3mf). Pre-fix this was the C2 false-success: rc=0
// with zero artifacts, no flag anywhere. Post-fix contract: run() returns
// false, exit_code()=6 (zero-product guard in run(), placed after the
// packaging gate so it never suppresses partial output), stats.success=false
// with the canonical no-plates message, and the EMPTY_PLATE warning still
// attributes the skip to the requested plate. No placeholder plate is
// fabricated for the warning — the failure classification comes from the
// zero-plate-results rule in finalise_statistics, not from the benign skip.
// ============================================================================
TEST_CASE("Requesting an empty plate: zero product fails with exit 6", "[integration][slice][fail][empty_plate]")
{
    // Pre-clean (stale-artifact trap): a leftover file would flip the
    // exists() assertion below without this run producing anything.
    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_empty_plate_requested.gcode.3mf", ec);

    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_empty_plate_skip.3mf", "empty_plate_requested",
                      OutputFormat::GCODE_3MF, /*single_plate=*/true, /*plate_id=*/2);

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);  // EXIT_PREPROCESS_ERROR (Types.hpp:16)
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    // The skip is attributed to the requested plate (0-based id 1).
    REQUIRE(has_plate_issue(r->engine->stats(), 1, "EMPTY_PLATE", IssueLevel::warning));

    // Warning-level orphan issues do not fabricate plate entries; the empty
    // stats.plates is what drives error_message in finalise_statistics.
    REQUIRE(r->engine->stats().plates.empty());
    REQUIRE(r->engine->stats().error_message == "No plates completed successfully");
}

// ============================================================================
// Case 17 — Multi-plate, ALL plates empty (slice_all_plates_empty_fail.3mf:
// both <plate> blocks lack model_instance references AND the build item is
// printable="0" — regenerated 2026-10-08, see P-23: with the geometric
// membership rebuild live, a printable="1" cube would be enrolled onto plate
// 1 and this case would flip to success). Same zero-product contract as
// Case 16, plus per-plate attribution of every skip.
// ============================================================================
TEST_CASE("All plates empty: zero product fails with exit 6 and both skips visible", "[integration][slice][fail][empty_plate]")
{
    // Pre-clean (stale-artifact trap), same rationale as Case 16.
    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_all_plates_empty.gcode.3mf", ec);

    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_all_plates_empty_fail.3mf", "all_plates_empty");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    // Both skips visible, each attributed to its own plate.
    REQUIRE(has_plate_issue(r->engine->stats(), 0, "EMPTY_PLATE", IssueLevel::warning));
    REQUIRE(has_plate_issue(r->engine->stats(), 1, "EMPTY_PLATE", IssueLevel::warning));
    REQUIRE(r->engine->stats().plates.empty());
    REQUIRE(r->engine->stats().error_message == "No plates completed successfully");
    // Zero-product observability (P-23 Fix B): the failure must be visible
    // at error level in the issue list, not only via error_message/exit code.
    REQUIRE(has_global_issue(r->engine->stats(), "NO_PLATES_PRODUCED", IssueLevel::error));
    // The rebuild ran but enrolled nothing (unprintable instance excluded),
    // so it must stay silent — no tip.
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "PLATE_MEMBERSHIP_REBUILT", IssueLevel::tip));
}

// ============================================================================
// Case 18 — Regression: object-level speed overrides delivered via
// different_settings_to_system must not crash Model::findMaxSpeed during
// brim generation. Production model 4e59b4a1a391c740.3mf (BBS project,
// 2 plates) carries support_speed (coFloats) etc. as object-level config;
// the unchecked dynamic_cast<ConfigOptionFloat*> in the single-arg
// opt_float(key) null-deref'd → SIGSEGV (exit 139) at "Generating skirt
// & brim" on unfixed libslic3r builds. Fixed in the OrcaSlicer fork
// (Model.cpp findMaxSpeed, commit 656353d876).
//
// Regressions locked in:
//   1. Both plates slice successfully — exit 0, success, no error issue
//      (no crash, no structured failure either).
//   2. The packaged gcode.3mf exists and is non-trivially sized.
// ============================================================================
TEST_CASE("Object-level speed override via different_settings_to_system: no findMaxSpeed crash, 2 plates ok", "[integration][slice][ok][findmax_speed]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/4e59b4a1a391c740.3mf", "findmax_speed_override");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE_FALSE(r->engine->any_error());

    REQUIRE(r->engine->stats().plates.size() == 2);
    for (const auto& plate : r->engine->stats().plates)
        REQUIRE(plate.success);

    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);
}

// ============================================================================
// Case 19 — P-08 positive control, paired with Case 9. Same geometry as the
// P-08 fixture 2c242c10f0e4b521.3mf (wipe tower y=244.697 → tower top 271.21
// past the 271 printable edge → exit 7), but re-saved through desktop Orca's
// IMPORT path, which discards the stored wipe_tower_x/y and auto-places a
// fresh tower (13/214.5). Locks in both halves of the P-08 contract:
//   1. With an in-bounds tower position the very same model slices clean —
//      exit 0, no TOOLPATH_OUTSIDE anywhere (the engine only ever rejected
//      the data, never the geometry).
//   2. The plate's toolpath_outside flag stays false end-to-end.
// See docs/precedents/wipe-tower-position-clamp.md (user-side workaround).
// ============================================================================
TEST_CASE("P-08 model re-saved via Orca import (regenerated tower) slices clean", "[integration][slice][ok][toolpath]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/wipe_tower_import_regen_ok.3mf", "wipe_tower_regen_ok");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE_FALSE(r->engine->any_error());

    REQUIRE(r->engine->stats().plates.size() == 1);
    const auto& p = r->engine->stats().plates[0];
    REQUIRE(p.success);
    REQUIRE_FALSE(p.toolpath_outside);

    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "TOOLPATH_OUTSIDE", IssueLevel::error));

    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);
}

// ============================================================================
// Case 20 — Plate-less 3MF (third-party exporter: no <plate> records, no
// plate thumbnails) slices as a SYNTHESIZED single plate holding every
// printable instance — the desktop Orca semantics (its GUI keeps the default
// plate and assigns instances geometrically). Verifies the D1''' fix:
//   1. exit 0, exactly one plate, that plate succeeds;
//   2. a PLATE_DATA_SYNTHESIZED tip marks the synthesis for cloud triage;
//   3. real output is produced.
// Golden reference: desktop Orca 2.4.0 sliced this very file to 1 object,
// 68 layers, 21.28 g — the filament bounds below guard against both an
// empty slice and a duplicated-instance regression around that value.
// ============================================================================
TEST_CASE("Plate-less 3MF slices as synthesized single plate", "[integration][slice][ok][plateless]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/plateless_single.3mf", "plateless_single");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE_FALSE(r->engine->any_error());

    REQUIRE(r->engine->stats().plates.size() == 1);
    const auto& p = r->engine->stats().plates[0];
    REQUIRE(p.success);

    // Desktop golden: 21.28 g single object — bracket it loosely.
    REQUIRE(p.total_filament_g > 5.0);
    REQUIRE(p.total_filament_g < 40.0);

    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_DATA_SYNTHESIZED", IssueLevel::tip));

    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);
}

// ============================================================================
// Case 21 — Plate-less 3MF whose single instance carries tz=48.613 (mesh
// locally centered around 0): exercises synthesized-plate + Z-bake Step-1
// in the same run. The baked model occupies world z[0,97.2] on the bed and
// must slice clean on the synthesized plate.
// ============================================================================
TEST_CASE("Plate-less 3MF with baked instance Z slices clean", "[integration][slice][ok][plateless]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/plateless_zbake.3mf", "plateless_zbake");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(r->engine->stats().plates.size() == 1);
    REQUIRE(r->engine->stats().plates[0].success);

    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_DATA_SYNTHESIZED", IssueLevel::tip));
    REQUIRE(boost::filesystem::exists(r->output_file));
}

// ============================================================================
// Case 22 — Plate-less 3MF whose only item is printable="0": the whole file
// has no printable instance. Desktop refuses to slice such a file upfront;
// the engine must hard-fail (exit 6, PLATELESS_NO_PRINTABLE_INSTANCES error)
// instead of the pre-fix silent exit-0-with-no-output.
// ============================================================================
TEST_CASE("Plate-less 3MF with all objects unprintable fails loudly", "[integration][slice][fail][plateless]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/plateless_unprintable.3mf", "plateless_unprintable");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);

    REQUIRE(has_global_issue(r->engine->stats(), "PLATELESS_NO_PRINTABLE_INSTANCES", IssueLevel::error));
    // Fix B suppression gate (P-23): the failure is already attributed by the
    // specific PLATELESS_NO_PRINTABLE_INSTANCES error, so the generic
    // NO_PLATES_PRODUCED must stay silent — single attribution.
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "NO_PLATES_PRODUCED", IssueLevel::error));
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));
}

// ============================================================================
// Case 23 — Plate-less 3MF with an out-of-bed object (obj4 xmin=0 vs bed
// xmin=0.5): the synthesized plate enrolls every printable instance, so the
// existing build-volume check classifies it PARTLY_OUTSIDE and fails hard.
// KNOWN DIVERGENCE (anchored, by design for now): desktop Orca slices the
// in-bed objects and only excludes the outside one; the engine rejects the
// whole task. Revisit with the "exclude + warn" follow-up, not silently.
// ============================================================================
TEST_CASE("Plate-less 3MF with out-of-bed object fails at build-volume", "[integration][slice][fail][plateless]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/plateless_oob_fail.3mf", "plateless_oob_fail");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);

    REQUIRE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_OUTSIDE_XY", IssueLevel::error));
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));
}

// ============================================================================
// Case 24 — Hand-crafted layer_config_ranges.xml (geom_above_bed_ok.3mf with
// Metadata/layer_config_ranges.xml injected). The 3MF importer validates
// nothing in that block, and libslic3r's layer_height_profile_from_ranges
// (Slicing.cpp) dereferences option("layer_height") without a null check —
// a range without that key used to kill the engine with SIGSEGV rc 139
// inside Print::validate (verified live 2026-10-08). The engine-side
// sanitize_layer_config_ranges() (read_3mf_model) must fix the range by
// falling back to the global default layer height so the slice completes,
// with a CONFIG_INVALID_LAYER_RANGE warning attributing the change.
// ============================================================================
TEST_CASE("Height range without layer_height no longer crashes; sanitized with warning", "[integration][slice][ok][layer_range]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/layer_range_missing_height.3mf", "lr_missing_height");

    // Pre-fix this was a SIGSEGV (rc 139); the whole regression contract is
    // reaching these assertions at all.
    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(boost::filesystem::exists(r->output_file));

    REQUIRE(has_global_issue(r->engine->stats(), "CONFIG_INVALID_LAYER_RANGE", IssueLevel::warning));
}

// ============================================================================
// Case 25 — Same injection vector, but the range is complete and instead
// carries wall/sparse_infill_filament=5 (above the 4 physical filaments).
// Height ranges support physical filaments only, and libslic3r silently
// clamps the id to extruder 1 (Print.cpp extruder collection) — the slice
// itself is unaffected, but the engine must surface the remap as a warning.
// Also locks in that such invalid data does NOT count as mixed-filament
// usage (the height-range path is deliberately absent from
// detect_mixed_filament_usage()).
// ============================================================================
TEST_CASE("Height range with out-of-range filament id slices with visible warning", "[integration][slice][ok][layer_range]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/layer_range_oob_filament.3mf", "lr_oob_filament");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(boost::filesystem::exists(r->output_file));

    REQUIRE(has_global_issue(r->engine->stats(), "CONFIG_INVALID_LAYER_RANGE", IssueLevel::warning));
    REQUIRE_FALSE(r->engine->stats().mixed_filament_used);
}

// ============================================================================
// Case 31 — The positive side of the Case 24/25 sanitizer contract (release-
// review blind spot 2026-10-10: "normal ranges not falsely warned" had zero
// assertions — [layerheight]8's fixture carries no ranges.xml, and the
// filcount multicolor fixture only exercises interior ids). The check is
// `id < 0 || id > num_physical` (sanitize_layer_config_ranges), so id ==
// num_physical is the off-by-one magnet: a `>=` typo would false-warn on the
// LAST physical slot and teach issue consumers to ignore the code. Fixture
// parks every legal boundary: wall=1 (first slot), sparse_infill=4 (==
// num_physical), solid_infill=0 (legal "default"), layer_height present.
// ============================================================================
TEST_CASE("Complete height ranges on the filament-id boundary stay warning-free", "[integration][slice][ok][layer_range]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/layer_range_valid_boundary.3mf", "lr_valid_boundary");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE(boost::filesystem::exists(r->output_file));

    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "CONFIG_INVALID_LAYER_RANGE", IssueLevel::warning));

    // The ranges must also take EFFECT — a sanitizer that dropped them would
    // pass the warning check vacuously. wall comes from slot 1 (engine id 0),
    // sparse infill from slot 4 (engine id 3): both must carry real usage.
    REQUIRE(!r->engine->stats().plates.empty());
    std::set<int> used;
    for (const auto& kv : r->engine->stats().plates[0].filament_used_g)
        used.insert(kv.first);
    REQUIRE(used.count(0) == 1);
    REQUIRE(used.count(3) == 1);
}

// ============================================================================
// Case 26 — Regression (P-23): MakerWorld / BambuStudio-02.06 / OrcaSlicer
// 2.4.2 exports author <plate> metadata but record no membership
// (<model_instance>) anywhere. Production incident 1e9d9919e7955ce4.3mf:
// every obj_inst_map empty -> "Marked 0 instances printable" -> EMPTY_PLATE
// warning -> exit 6 "No plates completed successfully" with a warnings-only
// JSON (invisible to issue-level consumers). Desktop 2.4.0 slices these fine
// because it ALWAYS rebuilds plate membership geometrically
// (PartPlateList::reload_all_objects; the obj_inst_map restore is commented
// out upstream). rebuild_plate_membership() ports that semantic.
//
// Regressions locked in:
//   1. Success path restored: exit 0, one plate, output produced.
//   2. The fallback is visible as a PLATE_MEMBERSHIP_REBUILT tip.
//   3. Weight sanity: loose 5-40 g bracket (the file's own embedded
//      slice_info golden is 21.04 g on the authoring printer; U1 official
//      preset substitution shifts it — same bracket style as Case 20).
// ============================================================================
TEST_CASE("MakerWorld 3MF without membership slices via geometric rebuild", "[integration][slice][ok][membership]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/1e9d9919e7955ce4.3mf", "mw_membership");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE_FALSE(r->engine->any_error());

    REQUIRE(r->engine->stats().plates.size() == 1);
    REQUIRE(r->engine->stats().plates[0].success);
    REQUIRE(r->engine->stats().plates[0].total_filament_g > 5.0);
    REQUIRE(r->engine->stats().plates[0].total_filament_g < 40.0);

    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_MEMBERSHIP_REBUILT", IssueLevel::tip));
    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);
}

// ============================================================================
// Case 27 — Membership rebuild with a benign empty sibling (P-23): same
// no-membership shape as Case 26 but two-plate, derived from Case 15's
// fixture by deleting the sole <model_instance> record. The printable cube
// (129.19, 183.71) sits in plate 1's grid cell, so the rebuild assigns it
// there; plate 2 legitimately has nothing and must keep the P-19 warning +
// skip semantics — one empty plate must not sink the rebuilt sibling.
// ============================================================================
TEST_CASE("Membership rebuild assigns the cube and skips the genuinely empty sibling", "[integration][slice][ok][membership]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_membership_rebuilt_2plate.3mf", "membership_2plate");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);

    REQUIRE(r->engine->stats().plates.size() == 1);
    REQUIRE(r->engine->stats().plates[0].success);

    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_MEMBERSHIP_REBUILT", IssueLevel::tip));
    REQUIRE(has_plate_issue(r->engine->stats(), 1, "EMPTY_PLATE", IssueLevel::warning));

    REQUIRE(boost::filesystem::exists(r->output_file));
}

// ============================================================================
// Case 28 — Production misaligned-membership 8-plate file (P-24
// always-rebuild): 2d986d79661ffb62.3mf carries NON-EMPTY plate maps that
// disagree with geometry by one grid cell (third-party exporter authored on
// a 260 mm bed grid; every object sits exactly on a cell centre, but the
// metadata claims neighbouring plates). P-23 Fix A trusted the maps and
// failed plates 3/5/8 with BUILD_VOLUME_FULLY_OUTSIDE (exit 6, no product —
// all-or-nothing), while desktop Orca 2.4.0 sliced all 8 plates: the desktop
// loader never restores membership from the file and always reassigns
// geometrically (reload_all_objects). rebuild_plate_membership() now ports
// that semantic wholesale (P-24), so this file must go fully green.
// Desktop cross-check: its gcode.3mf output orders the plates
// black/top/side/marcoLamparaAjustado/orangeQuestion/marcoLampara/
// marcoLamparaInferior/marcoLamparaSuperior — 8 plates, one object each.
// ============================================================================
TEST_CASE("Production 8-plate misaligned membership slices via always-rebuild", "[integration][slice][ok][membership]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/2d986d79661ffb62.3mf", "p24_misaligned8");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE_FALSE(r->engine->any_error());

    REQUIRE(r->engine->stats().plates.size() == 8);
    for (int i = 0; i < 8; ++i)
    {
        const auto& plate = r->engine->stats().plates[i];
        REQUIRE(plate.plate_id == i);
        REQUIRE(plate.success);
        REQUIRE(plate.total_filament_g > 0.0);
        REQUIRE(plate.print_time > 0.0f);
    }

    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_MEMBERSHIP_REBUILT", IssueLevel::tip));
    // Every object lands on some plate cell under the geometric assignment —
    // nothing may be dropped, and the old false positive must be gone.
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "INSTANCE_NOT_ON_ANY_PLATE", IssueLevel::warning));
    REQUIRE_FALSE(has_global_issue(r->engine->stats(), "BUILD_VOLUME_FULLY_OUTSIDE", IssueLevel::error));

    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);
}

// ============================================================================
// Case 29 — Off-grid instance with a healthy sibling plate (P-24
// non-blocking contract). slice_offgrid_sibling_ok.3mf has two
// membership-less plates: one cube instance dragged off every plate cell
// (world bbox X[-66.5,-39.5], entirely negative) and a second instance on
// plate 2's cell centre. The desktop puts the off-grid cube on its
// unprintable plate and slices the rest (fully-outside does not disable
// slicing — GLCanvas3D.cpp:3973); the engine equivalent is an
// INSTANCE_NOT_ON_ANY_PLATE warning that must NOT sink the good plate.
// The tip must also fire: plate 2's map went memberless -> populated, a
// real assignment change under the before/after diff.
// ============================================================================
TEST_CASE("Off-grid instance warns but sibling plate still slices", "[integration][slice][ok][membership]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_offgrid_sibling_ok.3mf", "offgrid_sibling_ok");

    REQUIRE(r->engine->stats().success);
    REQUIRE(r->engine->exit_code() == 0);
    REQUIRE_FALSE(r->engine->any_error());

    // Only the sibling produced; the off-grid instance belongs to no plate,
    // and plate 1's cell is empty -> benign skip, not a failure.
    REQUIRE(r->engine->stats().plates.size() == 1);
    REQUIRE(r->engine->stats().plates[0].plate_id == 1);
    REQUIRE(r->engine->stats().plates[0].success);
    REQUIRE(r->engine->stats().plates[0].total_filament_g > 0.0);

    REQUIRE(has_global_issue(r->engine->stats(), "INSTANCE_NOT_ON_ANY_PLATE", IssueLevel::warning));
    REQUIRE(has_plate_issue(r->engine->stats(), 0, "EMPTY_PLATE", IssueLevel::warning));
    REQUIRE(has_global_issue(r->engine->stats(), "PLATE_MEMBERSHIP_REBUILT", IssueLevel::tip));

    REQUIRE(boost::filesystem::exists(r->output_file));
    REQUIRE(boost::filesystem::file_size(r->output_file) > 1000);
}

// ============================================================================
// Case 30 — Sole instance off-grid (P-24 attribution contract).
// slice_offgrid_sole_fail.3mf: single plate, no member records, the only
// cube instance is off every plate cell. The drop itself is a warning —
// the exit-6 comes from the zero-product guard (NO_PLATES_PRODUCED,
// P-23 Fix B), NOT from the off-grid instance. This pins the failure
// classification so the warning can never silently harden into a blocker
// (that would re-create the reverse-parity defect P-24 removed).
// ============================================================================
TEST_CASE("Sole off-grid instance: warning plus zero-product exit 6", "[integration][slice][fail][membership]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/slice_offgrid_sole_fail.3mf", "offgrid_sole_fail");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 6);
    REQUIRE(r->engine->stats().plates.empty());

    REQUIRE(has_global_issue(r->engine->stats(), "INSTANCE_NOT_ON_ANY_PLATE", IssueLevel::warning));
    REQUIRE(has_plate_issue(r->engine->stats(), 0, "EMPTY_PLATE", IssueLevel::warning));
    REQUIRE(has_global_issue(r->engine->stats(), "NO_PLATES_PRODUCED", IssueLevel::error));

    // Nothing may be packaged for a zero-product run.
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));
}
