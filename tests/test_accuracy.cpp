/**
 * @file test_accuracy.cpp
 * @brief Regression baseline: complex multi-color AMS models slice correctly.
 *
 * These fixtures are not tied to a specific bug; they assert the engine
 * produces successful output on real-world multi-plate / multi-extruder models
 * with the expected per-plate extruder set, so a regression that breaks
 * multi-color slicing surfaces here.
 *
 * Outcomes measured 2026-08-07: all five small fixtures slice successfully
 * (exit 0), no error/serious_warning issues, with extruder sets matching each
 * model's color structure.
 * Tag: [integration][accuracy].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"
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
    cfg.output_base = std::string("/tmp/orca_test_acc_") + tag;

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

std::set<int> used_extruders(const SliceOutputStats::PlateStats& ps)
{
    std::set<int> s;
    for (const auto& kv : ps.filament_used_g) s.insert(kv.first);
    return s;
}

// Baseline contract: success, no error/serious_warning, non-zero filament.
void assert_healthy(const SliceEngine& e)
{
    REQUIRE(e.stats().success);
    REQUIRE(e.exit_code() == 0);
    REQUIRE_FALSE(e.stats().plates.empty());
    bool any_error = false;
    for (const auto& iss : e.stats().issues)
        if (iss.level == IssueLevel::error || iss.level == IssueLevel::serious_warning)
            any_error = true;
    REQUIRE_FALSE(any_error);
    for (const auto& ps : e.stats().plates)
    {
        REQUIRE(ps.success);
        REQUIRE(ps.total_filament_g > 0.0);
    }
}

std::string fixture(const char* name)
{
    return std::string(ORCA_TEST_FIXTURE_DIR) + "/accuracy/" + name;
}
} // namespace

TEST_CASE("Single-color models slice correctly", "[integration][accuracy]")
{
    auto bug = run_on(fixture("bug_hotel.3mf"), "bug");
    assert_healthy(*bug);
    REQUIRE(used_extruders(bug->stats().plates[0]) == std::set<int>{0});

    auto hinge = run_on(fixture("bistable_hinge.3mf"), "hinge");
    assert_healthy(*hinge);
    REQUIRE(used_extruders(hinge->stats().plates[0]) == std::set<int>{0});
}

TEST_CASE("Multi-plate model: per-plate extruder sets correct", "[integration][accuracy]")
{
    // bird_house: 3 plates — single(0), single(1), dual(0,1).
    auto e = run_on(fixture("bird_house.3mf"), "bird");
    assert_healthy(*e);
    REQUIRE(e->stats().plates.size() == 3);
    REQUIRE(used_extruders(e->stats().plates[0]) == std::set<int>{0});
    REQUIRE(used_extruders(e->stats().plates[1]) == std::set<int>{1});
    REQUIRE(used_extruders(e->stats().plates[2]) == std::set<int>{0, 1});
}

TEST_CASE("Multi-color AMS plate slices correctly", "[integration][accuracy]")
{
    // plant_pot: plate 1 uses three extruders (0,1,2) — a real AMS multi-color plate.
    auto e = run_on(fixture("plant_pot.3mf"), "plant");
    assert_healthy(*e);
    REQUIRE(e->stats().plates.size() == 2);
    REQUIRE(used_extruders(e->stats().plates[1]) == std::set<int>({0, 1, 2}));
}
