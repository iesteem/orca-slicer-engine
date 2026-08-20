/**
 * @file test_path_conflict.cpp
 * @brief Integration test for toolpath-conflict detection.
 *
 * Drives the full run() on fixtures whose objects are too close / overlap in
 * a way that produces G-code path conflicts. All measured outcomes (2026-08-07):
 * slicing produces a plate (run_ok true) but the result is unusable —
 * EXIT_POSTPROCESS_ERROR (7), any_error, success=false, and a TOOLPATH_CONFLICT
 * issue at serious_warning level. This verifies the validate-classification
 * unit tests (test_validate_classify.cpp) hold end-to-end: the conflict is
 * surfaced AND blocks output.
 *
 * Tag: [integration][pathconflict].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // IssueLevel, EXIT_POSTPROCESS_ERROR
#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

// Run full pipeline, output to /tmp. Returns engine for inspection.
std::unique_ptr<SliceEngine> run_on(const std::string& input_file, const char* tag)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = input_file;
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = std::string("/tmp/orca_test_pc_") + tag;

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

// True if an issue with the given code at the given level is present.
bool has_issue(const SliceOutputStats& s, const std::string& code, IssueLevel level)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == level)
            return true;
    return false;
}

// The common path-conflict outcome: a plate is produced but marked unusable,
// a TOOLPATH_CONFLICT serious_warning fires, and the pipeline exits with
// postprocess error. Asserts all of these.
void assert_toolpath_conflict(const SliceEngine& e)
{
    // Slicing ran far enough to produce a plate, but the result is unusable.
    REQUIRE(e.any_error());
    REQUIRE(e.any_postprocess_warning());
    REQUIRE(e.exit_code() == EXIT_POSTPROCESS_ERROR);
    REQUIRE_FALSE(e.stats().success);

    // The conflict is surfaced as a serious_warning.
    REQUIRE(has_issue(e.stats(), "TOOLPATH_CONFLICT", IssueLevel::serious_warning));
}
} // namespace

TEST_CASE("Toolpath conflict (multi-plate) blocks output", "[integration][pathconflict]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/path_conflict/toolpath_conflict_fail.3mf", "tp_fail");
    assert_toolpath_conflict(*e);
    // This fixture also escalates print-by-object collisions to error.
    REQUIRE(has_issue(e->stats(), "PRINT_BY_OBJECT_CAUTION", IssueLevel::error));
}

TEST_CASE("Toolpath conflict (self-overlap) blocks output", "[integration][pathconflict]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/path_conflict/conflict_serious_warning_2.3mf", "sw2");
    assert_toolpath_conflict(*e);
}

TEST_CASE("Toolpath conflict (frame/side) blocks output", "[integration][pathconflict]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/path_conflict/conflict_trigger_warning.3mf", "trig");
    assert_toolpath_conflict(*e);
}

TEST_CASE("Toolpath conflict (too close, 168 layers) blocks output", "[integration][pathconflict]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/path_conflict/too_close_84_fail.3mf", "close84");
    assert_toolpath_conflict(*e);
}

// --- Spiral-lift near boundary: large fixture, external + SKIP. -------------

TEST_CASE("Spiral-lift near boundary downgrades to warning, slices OK", "[integration][pathconflict]")
{
    // Fixture name predicts: a spiral-lift serious_warning is downgraded to a
    // plain warning and slicing succeeds. Large (~85MB) fixture, not in repo.
    //
    // NOTE: this case is SKIPPED when the fixture is absent (external path).
    // The assertions below follow the FILENAME expectation; on this dev host
    // the case was not verified end-to-end because the disk was too full to
    // slice an 85MB model (it failed with GCODE_EXPORT_ERROR, an environment
    // artifact, not an engine behavior). When run on a host with the fixture
    // and adequate disk, verify these hold.
    const char* external =
        "/home/joyx/Downloads/切片引擎测试-3MF文件包/path_conflict/"
        "新年摆件-U1，螺旋抬升的严重警告：SPIRAL_LIFT_NEAR_BOUNDARY降为普通警告，切片成功.3mf";
    if (!boost::filesystem::exists(external))
        SKIP("spiral-lift fixture not present (large, external); skipping");

    auto e = run_on(external, "spiral");
    // Slicing succeeds despite the spiral-lift notice (downgraded to warning).
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    REQUIRE(has_issue(e->stats(), "SPIRAL_LIFT_NEAR_BOUNDARY", IssueLevel::warning));
}


// --- 514d: 10-plate real-world project, 2 plates conflict (serious_warning). ---
// Largest mixed-outcome sample: plates 4 and 6 carry object collisions
// (TOOLPATH_CONFLICT serious_warning -> plate fails), the other 8 slice
// cleanly. Guards the serious_warning -> plate-failure propagation on a
// multi-plate project where most plates succeed, plus all-or-nothing
// packaging. Existing conflict fixtures are single-plate / all-fail shapes.
TEST_CASE("10-plate project: 2 conflict plates fail, 8 clean siblings succeed", "[integration][pathconflict][multiplate]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/path_conflict/multi_plate_conflict_514d.3mf", "p514d");

    assert_toolpath_conflict(*e);

    // Exactly the two conflicting plates fail; the clean eight do not.
    REQUIRE(e->stats().plates.size() == 10);
    int failed = 0;
    for (const auto& p : e->stats().plates) {
        bool has_conflict = false;
        for (const auto& iss : p.issues)
            if (iss.code == "TOOLPATH_CONFLICT")
                has_conflict = true;
        REQUIRE(p.success == !has_conflict);
        if (has_conflict) ++failed;
    }
    REQUIRE(failed == 2);

    // All-or-nothing: no gcode.3mf despite 8 successful plates.
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_test_pc_p514d.gcode.3mf"));
}
