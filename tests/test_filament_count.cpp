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

// --- Layer 2: golden comparison vs desktop OrcaSlicer (external + SKIP). ----
//
// The reference desktop outputs live at ~/Downloads/.../Orca/*.gcode.3mf. We
// cannot unzip them here without a zip dep, so the golden values are extracted
// from their slice_info.config by hand (filament used_g per plate) and pinned
// as constants, with the extraction date noted. The cloud engine's per-plate
// filament grams must match within a small tolerance (slicer rounding).

TEST_CASE("Filament grams match desktop OrcaSlicer reference (distinct_per_plate)", "[integration][filcount][golden]")
{
    // Desktop reference (extracted 2026-08-07 from
    // ~/Downloads/切片引擎测试-3MF文件包/filement_count/Orca/各盘都使用唯一的耗材...gcode.3mf
    // slice_info.config <filament used_g>): plates 1-4 = 5.92, 7.28, 5.92, 7.12 g.
    // The reference file must be present for this case to be meaningful; SKIP if not.
    const char* ref = "/home/joyx/Downloads/切片引擎测试-3MF文件包/filement_count/Orca/"
                      "各盘都使用唯一的耗材且各不相同，检查耗材统计正确，json结果和Gcode要一致.gcode.3mf";
    if (!boost::filesystem::exists(ref))
        SKIP("desktop OrcaSlicer reference gcode not present (external); skipping golden check");

    auto e = run_on(fixture("distinct_per_plate.3mf"), "distinct_golden");
    REQUIRE(e->stats().success);
    REQUIRE(e->stats().plates.size() == 4);

    // Cloud vs desktop, 0.05g tolerance (slicer rounding).
    const double ref_g[4] = {5.92, 7.28, 5.92, 7.12};
    for (size_t i = 0; i < 4; ++i)
    {
        INFO("plate " << i << " cloud=" << e->stats().plates[i].total_filament_g
                      << " desktop=" << ref_g[i]);
        REQUIRE(e->stats().plates[i].total_filament_g == Approx(ref_g[i]).margin(0.05));
    }
}
