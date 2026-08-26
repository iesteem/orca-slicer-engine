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
    explicit PathCleanup(std::string p) : path(std::move(p)) {}
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
    cfg.skip_preset_substitution = false;
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
    cfg.skip_preset_substitution = false;
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
// Case 9 — wipe-tower-induced TOOLPATH_OUTSIDE. Small single-plate fixture
// (擦除塔gcode路径超限.3mf, renamed wipe_tower_toolpath_outside.3mf): the
// object itself sits inside the build volume, but its wipe tower + brim
// extrude past the bed edge. Guards the compute_toolpath_outside port on the
// minimal single-plate, plate-origin-at-zero shape (the Case 8 fixture covers
// the multi-plate grid-offset variant). Verified live: exit 7, TOOLPATH_OUTSIDE
// error on plate 1, package suppressed.
// ============================================================================
TEST_CASE("Wipe tower toolpath outside bed detected post-slice (single plate)", "[integration][slice][fail][toolpath]")
{
    auto r = run_full(ORCA_TEST_FIXTURE_DIR "/wipe_tower_toolpath_outside.3mf", "wipe_tower_outside");

    REQUIRE_FALSE(r->engine->stats().success);
    REQUIRE(r->engine->any_error());
    REQUIRE(r->engine->exit_code() == 7);
    REQUIRE_FALSE(boost::filesystem::exists(r->output_file));

    // Single plate: it must carry the flag, the error issue, and fail.
    REQUIRE(r->engine->stats().plates.size() == 1);
    const auto& p = r->engine->stats().plates[0];
    REQUIRE(p.toolpath_outside);
    REQUIRE_FALSE(p.success);
    bool has_issue = false;
    for (const auto& iss : p.issues)
        if (iss.code == "TOOLPATH_OUTSIDE" && iss.level == IssueLevel::error)
            has_issue = true;
    REQUIRE(has_issue);
    REQUIRE(has_global_issue(r->engine->stats(), "TOOLPATH_OUTSIDE", IssueLevel::error));
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
