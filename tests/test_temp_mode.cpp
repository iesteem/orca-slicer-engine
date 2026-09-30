/**
 * @file test_temp_mode.cpp
 * @brief Integration test for temperature-mode / high-low-temp mixing rules.
 *
 * Two outcome classes (measured 2026-08-07, matching fixture filenames):
 *   - Mixing high-temp and low-temp filaments (ABS+PLA) on one plate is
 *     rejected: FILAMENT_TEMP_MIXING error, exit 6 (preprocess).
 *   - A single-filament print with a specific chamber/fan mode (MODE=3 / 1)
 *     slices normally: exit 0.
 * Tag: [integration][tempmode].
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
    cfg.output_base = std::string("/tmp/orca_test_tm_") + tag;

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

std::string fixture(const char* name)
{
    return std::string(ORCA_TEST_FIXTURE_DIR) + "/temp_mode/" + name;
}
} // namespace

// --- Failure: high-temp / low-temp filaments mixed on a plate. -------------

TEST_CASE("ABS+PLA multi-plate high-low-temp mix fails", "[integration][tempmode]")
{
    auto e = run_on(fixture("mixed_abs_pla_fail.3mf"), "mixed");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->any_error());
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("PLA+ABS with tower conflict fails on temp mix", "[integration][tempmode]")
{
    auto e = run_on(fixture("pla_abs_tower_conflict_fail.3mf"), "tower");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("PLA+ABS high-low-temp mix fails", "[integration][tempmode]")
{
    auto e = run_on(fixture("pla_abs_mixed_fail.3mf"), "plxabs");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

// --- Success: single filament with a specific mode setting. ----------------

TEST_CASE("ABS chamber-warm MODE=3 slices OK", "[integration][tempmode]")
{
    auto e = run_on(fixture("abs_mode3.3mf"), "abs_m3");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
}

TEST_CASE("PETG weak-cool MODE=3 slices OK", "[integration][tempmode]")
{
    auto e = run_on(fixture("petg_mode3.3mf"), "petg_m3");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
}

TEST_CASE("PLA strong-cool MODE=1 slices OK", "[integration][tempmode]")
{
    auto e = run_on(fixture("pla_mode1.3mf"), "pla_m1");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
}
