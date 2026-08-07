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
