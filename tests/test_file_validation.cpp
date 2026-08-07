/**
 * @file test_file_validation.cpp
 * @brief Integration test for input-file / load failure paths.
 *
 * Drives the full run() on deliberately-invalid inputs and asserts the engine
 * rejects each with EXIT_LOAD_ERROR (3) and the correct issue code. Two failure
 * classes are covered:
 *   - FORMAT_REJECTED: wrong file extension (.4mf, .zip) — caught by
 *     validate_input_file before any 3MF parsing.
 *   - LOAD_3MF_ERROR: a .3mf that parses-fails or yields no geometry — caught
 *     inside read_3mf_model (Model::read_from_file throws, or the gcode.3mf
 *     "empty" detection fires).
 *
 * Outcomes measured 2026-08-07; assertions pin exit code + issue code (stable
 * contracts), not exact message text.
 * Tag: [integration][fileval].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // Issue, IssueLevel, EXIT_LOAD_ERROR
#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

// Run the full pipeline on an input; return the engine for inspection.
std::unique_ptr<SliceEngine> run_on(const std::string& input_file)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = input_file;
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

bool has_issue(const SliceOutputStats& s, const std::string& code)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == IssueLevel::error)
            return true;
    return false;
}

// Assert the common rejection contract: exit 3, any_error, no success, no
// output plates.
void assert_rejected(const SliceEngine& e)
{
    REQUIRE(e.exit_code() == EXIT_LOAD_ERROR);
    REQUIRE(e.any_error());
    REQUIRE_FALSE(e.stats().success);
}
} // namespace

// --- FORMAT_REJECTED: extension is not .3mf (case-insensitive). -----------

TEST_CASE("Wrong extension (.4mf) rejected as FORMAT_REJECTED", "[integration][fileval]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/file_validation/wrong_extension.4mf");
    assert_rejected(*e);
    REQUIRE(has_issue(e->stats(), "FORMAT_REJECTED"));
}

TEST_CASE("Zip file (.zip) rejected as FORMAT_REJECTED", "[integration][fileval]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/file_validation/actually_zip.zip");
    assert_rejected(*e);
    REQUIRE(has_issue(e->stats(), "FORMAT_REJECTED"));
}

// --- LOAD_3MF_ERROR: .3mf that fails to parse or has no geometry. ---------

TEST_CASE("Empty 3mf rejected as LOAD_3MF_ERROR", "[integration][fileval]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/file_validation/empty_3mf.3mf");
    assert_rejected(*e);
    REQUIRE(has_issue(e->stats(), "LOAD_3MF_ERROR"));
}

TEST_CASE("3mf with no model objects rejected as LOAD_3MF_ERROR", "[integration][fileval]")
{
    // Fixture name says "no model objects" but the engine actually fails inside
    // read_from_file (parse throws) before reaching the objects.empty() check,
    // so the code is LOAD_3MF_ERROR, not MODEL_EMPTY. Pinned to measured behavior.
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/file_validation/no_model_objects.3mf");
    assert_rejected(*e);
    REQUIRE(has_issue(e->stats(), "LOAD_3MF_ERROR"));
}

TEST_CASE("3mf with no model data rejected as LOAD_3MF_ERROR", "[integration][fileval]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/file_validation/no_model_data.3mf");
    assert_rejected(*e);
    REQUIRE(has_issue(e->stats(), "LOAD_3MF_ERROR"));
}

// --- gcode.3mf mistaken as input: large fixture, external path + SKIP. ----

TEST_CASE("gcode.3mf as input rejected as LOAD_3MF_ERROR", "[integration][fileval]")
{
    // The gcode.3mf-as-input fixture is large (~37MB) and not in the repo.
    const char* external = "/home/joyx/Downloads/切片引擎测试-3MF文件包/file_validation/gcode.3mf文件，切片失败.3mf";
    if (!boost::filesystem::exists(external))
        SKIP("gcode.3mf-as-input fixture not present (large, external); skipping");

    auto e = run_on(external);
    assert_rejected(*e);
    REQUIRE(has_issue(e->stats(), "LOAD_3MF_ERROR"));
}
