/**
 * @file test_unsupported.cpp
 * @brief Integration test for unsupported-feature rejection.
 *
 * Two cases (measured 2026-08-07):
 *   - Print-by-object on a fixture that would collide: rejected with
 *     PRINT_BY_OBJECT_CAUTION error, exit 6.
 *   - Variable layer height with organic supports: rejected with
 *     ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT error, exit 7 (postprocess).
 * Tag: [integration][unsupported].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // IssueLevel, EXIT_PREPROCESS_ERROR, EXIT_POSTPROCESS_ERROR
#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

std::unique_ptr<SliceEngine> run_on(const std::string& input_file, const char* tag)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = input_file;
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = std::string("/tmp/orca_test_uns_") + tag;

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

bool has_error_issue(const SliceOutputStats& s, const std::string& code)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == IssueLevel::error)
            return true;
    return false;
}
} // namespace

TEST_CASE("Print-by-object collision rejected", "[integration][unsupported]")
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/unsupported/print_sequential_fail.3mf", "pbo");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "PRINT_BY_OBJECT_CAUTION"));
}
// (Variable-layer-height case removed 2026-08-25: fixture lived in the external pool that no longer exists. ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT classification stays covered by test_validate_classify unit tests.)
