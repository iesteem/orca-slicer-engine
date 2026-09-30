/**
 * @file test_filament_count.cpp
 * @brief Integration test for per-plate / per-extruder filament usage statistics.
 *
 * Two layers of verification:
 *   1. Extruder-set correctness — each plate uses the extruders it should,
 *      guarding the historical [[modifier-extruder-trim-bug]] (multi-color
 *      plates mis-trimmed to single color) and [[wipe-tower-layer-config-ranges-bug]].
 *   2. Golden comparison — cloud-engine filament grams match the desktop
 *      OrcaSlicer slice_info.config used_g for the same fixture (the Orca/
 *      reference outputs live under ~/Downloads, external + SKIP). This
 *      guards [[filament-stats-fix-verified]].
 *
 * Tag: [integration][filcount].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"
#include "SliceEngine.hpp"

using Catch::Approx;

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
    cfg.output_base = std::string("/tmp/orca_test_fc_") + tag;

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

// Set of extruder IDs that a plate used (from filament_used_g keys).
std::set<int> used_extruders(const SliceOutputStats::PlateStats& ps)
{
    std::set<int> s;
    for (const auto& kv : ps.filament_used_g) s.insert(kv.first);
    return s;
}

std::string fixture(const char* name)
{
    return std::string(ORCA_TEST_FIXTURE_DIR) + "/filement_count/" + name;
}
} // namespace

// --- Layer 1: extruder-set correctness (historical bug guards). ------------

TEST_CASE("Distinct filament per plate: each plate uses exactly one extruder", "[integration][filcount]")
{
    // 4 plates, each a different single filament. Guards [[modifier-extruder-trim-bug]]:
    // a correct run reports one extruder per plate, not a mis-trimmed single color.
    auto e = run_on(fixture("distinct_per_plate.3mf"), "distinct");
    REQUIRE(e->stats().success);
    REQUIRE(e->stats().plates.size() == 4);

    REQUIRE(used_extruders(e->stats().plates[0]) == std::set<int>{0});
    REQUIRE(used_extruders(e->stats().plates[1]) == std::set<int>{1});
    REQUIRE(used_extruders(e->stats().plates[2]) == std::set<int>{2});
    REQUIRE(used_extruders(e->stats().plates[3]) == std::set<int>{3});
}

TEST_CASE("Layer-range multicolor: single plate uses two extruders", "[integration][filcount]")
{
    // Height-range color change on one plate -> two extruders. Guards
    // [[wipe-tower-layer-config-ranges-bug]] (range-based colors must count).
    auto e = run_on(fixture("layer_range_multicolor.3mf"), "layerrange");
    REQUIRE(e->stats().success);
    REQUIRE(e->stats().plates.size() == 1);
    REQUIRE(used_extruders(e->stats().plates[0]) == std::set<int>{0, 2});
}

TEST_CASE("Seven filaments / five plates: plate 0 uses three extruders", "[integration][filcount]")
{
    // Fixture: plate 1 (idx 0) uses three filaments; others single. Guards the
    // multi-extruder counting path on a 5-plate / 7-filament mix.
    auto e = run_on(fixture("seven_filaments_a.3mf"), "seven_a");
    REQUIRE(e->stats().success);
    REQUIRE(e->stats().plates.size() >= 1);
    REQUIRE(used_extruders(e->stats().plates[0]) == std::set<int>({0, 2, 4}));
}

// (Golden grams-vs-desktop case removed 2026-08-25: the desktop reference gcode lived in the external pool that no longer exists on this machine. Gram-exact comparison needs a fresh desktop export to return.)

// --- Dual-plate modifier multicolor (妞妞球同类型模型). ------------------------
// Two plates, each object carrying a modifier volume assigning a second
// filament: plate 1 uses slots 1+2, plate 2 uses slots 3+4 (disjoint pairs —
// per-plate filament assignment, not shared slots). Guards
// [[modifier-extruder-trim-bug]] (multi-color plates mis-trimmed to a single
// color) on the dual-plate modifier shape, which the single-plate fixtures
// above do not cover. Verified live: both plates slice OK, 2 filaments each.
TEST_CASE("Dual-plate modifier multicolor: each plate keeps both extruders", "[integration][filcount]")
{
    auto e = run_on(fixture("modifier_multicolor_dual_plate.3mf"), "modifier_dual");
    REQUIRE(e->stats().success);
    REQUIRE(e->stats().plates.size() == 2);

    // Plate 1: extruder set {0, 1} (0-based engine ids; JSON reports 1-based).
    REQUIRE(used_extruders(e->stats().plates[0]) == std::set<int>{0, 1});
    // Plate 2: extruder set {2, 3} — disjoint from plate 1.
    REQUIRE(used_extruders(e->stats().plates[1]) == std::set<int>{2, 3});

    // Both filaments on each plate carry real usage — the modifier region
    // actually extruded, not a zero-gram phantom slot.
    for (const auto& ps : e->stats().plates)
        for (const auto& kv : ps.filament_used_g)
            REQUIRE(kv.second > 0.0);
}

// --- Mushroom flush-matrix regression (【Lucas造物】迷你蘑菇). -----------------
// Real-world model that historically crashed with SIGSEGV inside
// ToolOrdering: the 3MF ships a flush_volumes_matrix whose dimension
// (3×3) doesn't match the filament slot count (4). normalize_flush_volumes_matrix
// (SliceEngine.cpp) repairs it before slicing; without that repair the engine
// crashes instead of failing gracefully. Guards [[mushroom-toolordering-sigsegv]].
TEST_CASE("Mushroom model with mismatched flush matrix slices without crash", "[integration][filcount][flush]")
{
    auto e = run_on(fixture("mushroom_flush_matrix.3mf"), "mushroom");
    // The historical failure was a hard SIGSEGV (process death) — reaching
    // these assertions at all proves the crash is gone.
    REQUIRE(e->stats().success);
    REQUIRE(e->stats().plates.size() == 1);

    // Single plate, three filaments in use (0-based ids {1, 2, 3}), all with
    // real usage — multi-color toolpath survived flush-matrix repair.
    REQUIRE(used_extruders(e->stats().plates[0]) == std::set<int>{1, 2, 3});
    for (const auto& kv : e->stats().plates[0].filament_used_g)
        REQUIRE(kv.second > 0.0);
}
