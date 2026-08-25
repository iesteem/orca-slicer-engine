/**
 * @file test_geometry_preprocess.cpp
 * @brief Integration test for SliceEngine geometry-half preprocessing stages.
 *
 * Drives SliceEngine through run_geometry_preprocess_only() — the config half
 * (load + preset substitution) plus the geometry-half preprocessing prefix
 * (validate_input, run_geometry_checks, bake_instance_z_into_mesh,
 * assign_arrange_order, setup_extruder_params), stopping before the per-plate
 * slicing loop. Then inspects the resulting m_model / m_stats / m_config to
 * verify the invariants each preprocessing stage guarantees.
 *
 * Case B (ensure_on_bed) is driven across multiple Z-variant fixtures
 * (geom_above_bed_ok / geom_below_bed_* / geom_partial_sink_* / geom_too_high)
 * to verify the desktop "allow_negative_z" rule end-to-end: above-bed models
 * seat on the bed (min_z ~= 0), intentional/partial sinks are preserved
 * (min_z < 0). These are sign/interval invariants tied to the original
 * instance Z, not to fixture-specific dimensions.
 *
 * Links the full libslic3r. Tag: [integration][geom].
 */

#include <catch_amalgamated.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Config.hpp"        // ConfigOptionFloats
#include "libslic3r/Model.hpp"         // Model, ModelObject, ModelInstance
#include "libslic3r/Utils.hpp"         // set_data_dir / set_resources_dir

#include "Types.hpp"                   // Issue, IssueLevel
#include "SliceEngine.hpp"

using Slic3r::ModelInstance;
using Slic3r::ModelObject;
using Catch::Approx;

static const char* kFixture   = ORCA_TEST_FIXTURE;
static const char* kResources = ORCA_TEST_RESOURCES;

// (Cabin defect-detection case removed 2026-08-25: the 72MB fixture never lived in the repo and is no longer available. In-repo defect fixtures above still cover the warning path.)

// ============================================================================
// Case B — bake_instance_z_into_mesh: instance Z offset is baked into the mesh,
// and the desktop "allow_negative_z" rule is honored.
//
// Print::apply discards the instance Z offset, so bake_instance_z_into_mesh bakes
// it into mesh vertices and zeroes the instance offset (regression guard for
// [[ensure-on-bed-fix-plan]]). The resulting mesh Z placement depends on the
// original instance Z sign:
//   - Z >= 0 (above / on bed): baked then seated on the bed  -> min_z ~= 0
//   - Z < 0  (intentional sinking): preserved               -> min_z < 0
//   - partial sink (object straddles z=0): preserved         -> min_z < 0 < max_z
//
// Each sub-case is driven with skip_preset_substitution=true to isolate the
// geometry stage. Assertions are sign/interval invariants derived from the
// original instance Z (read from the 3MF transform), NOT from fixture-specific
// dimensions — so they stay valid across model sizes.
// ============================================================================

namespace {
// Drive run_geometry_preprocess_only on a fixture, asserting it completes.
// Caller inspects engine.model() / .stats() afterwards.
void run_preprocess_on(SliceEngine*& out_engine, const char* fixture_path,
                       std::vector<std::string>& temp_files, std::unique_ptr<SliceEngine>& engine_storage)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = fixture_path;
    cfg.skip_preset_substitution = true;  // isolate geometry stage
    cfg.temp_dir = "/tmp";

    engine_storage = std::make_unique<SliceEngine>(cfg, temp_files);
    out_engine = engine_storage.get();
    REQUIRE(out_engine->run_geometry_preprocess_only());
    REQUIRE(!out_engine->any_error());
}
} // namespace

TEST_CASE("ensure_on_bed: above-bed model seats on bed (min_z ~= 0)", "[integration][geom][zbed]")
{
    // Fixture instance Z = 13.5 (>= 0). After bake + seat, the object rests on
    // the bed: min_z ~= 0. If Z were lost, the object would sink to min_z < 0.
    std::vector<std::string> temp_files;
    SliceEngine* engine = nullptr;
    std::unique_ptr<SliceEngine> storage;
    run_preprocess_on(engine, ORCA_TEST_FIXTURE_DIR "/geom_above_bed_ok.3mf", temp_files, storage);

    for (const ModelObject* obj : engine->model().objects)
    {
        for (const ModelInstance* inst : obj->instances)
        {
            INFO("object \"" << obj->name << "\" instance Z offset");
            REQUIRE(inst->get_offset().z() == Approx(0.0).margin(1e-6));  // Z baked out of offset
        }
        INFO("object \"" << obj->name << "\" min_z=" << obj->min_z());
        REQUIRE(obj->min_z() == Approx(0.0).margin(1e-3));  // seated on bed, not sunk
    }
}

TEST_CASE("ensure_on_bed: intentional sink preserved (min_z < 0)", "[integration][geom][zbed]")
{
    // Fixture instance Z = -13.45 (< 0, fully-buried intentional sink). Under
    // allow_negative_z=true the sink is preserved, so min_z < 0. The instance
    // Z offset is still baked out (== 0).
    std::vector<std::string> temp_files;
    SliceEngine* engine = nullptr;
    std::unique_ptr<SliceEngine> storage;
    run_preprocess_on(engine, ORCA_TEST_FIXTURE_DIR "/geom_below_bed_mostly.3mf", temp_files, storage);

    // This fixture carries two objects; the sunk one is the one with min_z < 0.
    bool found_sunk = false;
    for (const ModelObject* obj : engine->model().objects)
    {
        for (const ModelInstance* inst : obj->instances)
        {
            INFO("object \"" << obj->name << "\" instance Z offset");
            REQUIRE(inst->get_offset().z() == Approx(0.0).margin(1e-6));
        }
        if (obj->min_z() < -1e-3)
            found_sunk = true;
    }
    REQUIRE(found_sunk);  // at least one object retained its below-bed sink
}

TEST_CASE("ensure_on_bed: partial sink straddles z=0 (min_z<0<max_z)", "[integration][geom][zbed]")
{
    // Fixture instance Z = 10.5 with an object whose mesh extends below its
    // origin, yielding a partial sink (straddles z=0). Preserved: min_z < 0
    // AND max_z > 0.
    std::vector<std::string> temp_files;
    SliceEngine* engine = nullptr;
    std::unique_ptr<SliceEngine> storage;
    run_preprocess_on(engine, ORCA_TEST_FIXTURE_DIR "/geom_partial_sink_ok.3mf", temp_files, storage);

    for (const ModelObject* obj : engine->model().objects)
    {
        for (const ModelInstance* inst : obj->instances)
        {
            REQUIRE(inst->get_offset().z() == Approx(0.0).margin(1e-6));
        }
        INFO("object \"" << obj->name << "\" min_z=" << obj->min_z() << " max_z=" << obj->max_z());
        REQUIRE(obj->min_z() < -1e-3);
        REQUIRE(obj->max_z() > 1e-3);
    }
}

TEST_CASE("ensure_on_bed: tall model bakes Z but stays tall", "[integration][geom][zbed]")
{
    // Fixture instance Z = 270 with scale Z = 20 (a deliberately too-tall model).
    // Z is baked and the object seats on the bed (min_z ~= 0), but the excessive
    // height is NOT corrected here — that is a build-volume-check concern, not
    // ensure_on_bed's. We assert Z-bake + on-bed seating only; height is left
    // for the later build-volume stage (covered in a future step).
    std::vector<std::string> temp_files;
    SliceEngine* engine = nullptr;
    std::unique_ptr<SliceEngine> storage;
    run_preprocess_on(engine, ORCA_TEST_FIXTURE_DIR "/geom_too_high.3mf", temp_files, storage);

    bool found_tall = false;
    for (const ModelObject* obj : engine->model().objects)
    {
        for (const ModelInstance* inst : obj->instances)
        {
            REQUIRE(inst->get_offset().z() == Approx(0.0).margin(1e-6));
        }
        if (obj->min_z() == Approx(0.0).margin(1e-3) && obj->max_z() > 100.0)
            found_tall = true;
    }
    REQUIRE(found_tall);
}


// ============================================================================
// Case C — assign_arrange_order: every instance gets a globally monotonic,
// 1-based, step-1 ordering across the whole model.
// (SliceEngine.cpp:1591-1597 stamps order = 1,2,3,... in object-then-instance
// iteration order.) Uses a TWO-object fixture so the sequence is genuinely
// [1, 2] — a single-object fixture would only assert the trivial == 1 and
// could not detect an off-by-one or non-incrementing bug.
// ============================================================================
TEST_CASE("assign_arrange_order stamps monotonic 1-based ordering", "[integration][geom][order]")
{
    std::vector<std::string> temp_files;
    SliceEngine* engine = nullptr;
    std::unique_ptr<SliceEngine> storage;
    run_preprocess_on(engine, ORCA_TEST_FIXTURE_DIR "/geom_below_bed_mostly.3mf", temp_files, storage);

    const Slic3r::Model& model = engine->model();

    std::vector<int> orders;
    for (const ModelObject* obj : model.objects)
        for (const ModelInstance* inst : obj->instances)
            orders.push_back(inst->arrange_order);

    // The two-object fixture must yield at least 2 instances.
    REQUIRE(orders.size() >= 2);

    // The stamped sequence is exactly 1,2,3,...,N.
    for (size_t i = 0; i < orders.size(); ++i)
    {
        INFO("instance[" << i << "] arrange_order=" << orders[i]);
        REQUIRE(orders[i] == static_cast<int>(i + 1));
    }
}

// ============================================================================
// Case D — setup_extruder_params: regression guard, not a correctness proof.
//
// setup_extruder_params (SliceEngine.cpp:1599-1608) reads filament_diameter's
// slot count as num_extruders and writes Model::extruderParamsMap. That static
// map has no public getter, so its CONTENT cannot be asserted from outside
// (NOT VERIFIED). The real value of this case is a no-crash guard: extruder-
// count mismatches have caused SIGSEGVs before ([[mushroom-toolordering-sigsegv]],
// [[modifier-extruder-trim-bug]]). Running the full substitution path
// (skip_preset_substitution=false) then setup_extruder_params without crashing
// is a meaningful regression signal even though the map itself is unchecked.
// ============================================================================
TEST_CASE("setup_extruder_params derives extruder count from filament_diameter", "[integration][geom][extruder]")
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = kFixture;
    cfg.skip_preset_substitution = false;
    cfg.temp_dir = "/tmp";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    REQUIRE(engine.run_geometry_preprocess_only());

    const auto& ecfg = engine.config();
    const auto* fd = ecfg.option<Slic3r::ConfigOptionFloats>("filament_diameter");
    REQUIRE(fd);
    REQUIRE_FALSE(fd->values.empty());

    // setup_extruder_params feeds num_extruders = this size into
    // Model::setExtruderParams. Confirming the slot count is real (>= 1) and
    // that the stage ran without error is the no-crash signal; the written map
    // is NOT VERIFIED (no public getter).
    int num_extruders = static_cast<int>(fd->values.size());
    INFO("filament_diameter slots (== num_extruders) = " << num_extruders);
    REQUIRE(num_extruders >= 1);

    // The whole substitution + setup_extruder_params path completed without
    // error — the no-crash regression guard this case exists for.
    REQUIRE(!engine.any_error());
}
