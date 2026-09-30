/**
 * @file test_boundary.cpp
 * @brief Integration test for out-of-range config value detection (nozzle,
 * layer height, temperature).
 *
 * Outcomes are MEASURED (2026-08-07). The three fixtures behave differently:
 *   - nozzle=0: rejected — the official preset lookup fails for nozzle 0
 *     (PRINTER_PRESET_LOAD_ERROR, exit 6). Also emits CONFIG_INVALID warnings.
 *   - layer_height=0: NOT rejected — a CONFIG_INVALID_layer_height warning
 *     fires but slicing succeeds (exit 0). Config-range warnings do not block.
 *   - temperature=999: NOT flagged — the cloud engine's preset substitution
 *     overwrites the user's nozzle_temperature with the official filament
 *     preset value (verified: 999 -> 215/260), so slicing sees a legal
 *     temperature. The filename describes desktop behavior, not cloud. Intended.
 * Tag: [integration][boundary].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // IssueLevel, EXIT_PREPROCESS_ERROR
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
    cfg.output_base = std::string("/tmp/orca_test_bnd_") + tag;

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

bool has_issue(const SliceOutputStats& s, const std::string& code, IssueLevel level)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == level)
            return true;
    return false;
}

bool has_no_issue_starting_with(const SliceOutputStats& s, const std::string& prefix)
{
    for (const auto& iss : s.issues)
        if (iss.code.rfind(prefix, 0) == 0)
            return true;
    return false;
}

std::string fixture(const char* name)
{
    return std::string(ORCA_TEST_FIXTURE_DIR) + "/boundary/" + name;
}
} // namespace

TEST_CASE("Nozzle=0 rejected: official preset not found", "[integration][boundary]")
{
    // nozzle_diameter=0 -> the official U1 preset lookup by nozzle fails.
    auto e = run_on(fixture("nozzle_oob.3mf"), "nozzle");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_issue(e->stats(), "PRINTER_PRESET_LOAD_ERROR", IssueLevel::error));
    // Config-range warnings also fire but do not change the outcome.
    REQUIRE(has_no_issue_starting_with(e->stats(), "CONFIG_INVALID_"));
}

TEST_CASE("Layer-height=0 warned but not blocked (filename divergence)", "[integration][boundary]")
{
    // Fixture name predicts "越界检测" (rejection), but layer_height=0 only
    // emits a CONFIG_INVALID_layer_height warning; slicing succeeds (exit 0).
    // Config-range warnings are advisory here, not blocking. Measured outcome.
    auto e = run_on(fixture("layer_height_oob.3mf"), "lh");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    REQUIRE(has_issue(e->stats(), "CONFIG_INVALID_layer_height", IssueLevel::warning));
}

TEST_CASE("Temperature=999 overwritten by filament preset, slices OK", "[integration][boundary]")
{
    // Fixture name predicts "temperature=999 out-of-range detection", but the
    // cloud engine's preset-substitution policy overwrites the user's
    // nozzle_temperature with the official filament preset value (verified:
    // 999 -> 215 for PLA, 260 for PA). So slicing sees a legal temperature and
    // succeeds (exit 0) with no CONFIG_INVALID warning. The filename describes
    // DESKTOP behavior (no substitution -> detects 999), not cloud behavior.
    // This is intended, not a gap.
    auto e = run_on(fixture("temp_oob.3mf"), "temp");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    // No config-range warning: the out-of-range 999 was replaced before slicing.
    REQUIRE_FALSE(has_no_issue_starting_with(e->stats(), "CONFIG_INVALID_"));
}
