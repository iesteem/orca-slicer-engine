/**
 * @file test_temp_mode.cpp
 * @brief Integration test for temperature-mode / high-low-temp mixing rules.
 *
 * Three outcome classes (measured 2026-08-07 / 2026-09-18, matching fixture
 * filenames):
 *   - Mixing high-temp and low-temp filaments (ABS+PLA) on one plate is
 *     rejected: FILAMENT_TEMP_MIXING error, exit 6 (preprocess).
 *   - A single-filament print with a specific chamber/fan mode (MODE=3 / 1)
 *     slices normally: exit 0.
 *   - [red] DESIRED-behaviour cases (2026-09-18): the ABS slot is referenced
 *     only by an object-level / part-level feature-filament key or a
 *     modifier's extruder. Desktop Plater.cpp:22250-22276 collects filament
 *     slots from plate config, object config AND all volumes (modifiers
 *     included); SliceEngine::check_filament_temp_mixing() only reads
 *     model-part extruders and global keys, so these mixings are real but
 *     currently waved through (review 2026-09-16, item C1). The [red] cases
 *     pin the desktop-parity contract and FAIL until the collector is
 *     aligned; run `engine-integration "~[red]"` for a green pass.
 * Tag: [integration][tempmode].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // IssueLevel, EXIT_PREPROCESS_ERROR
#include "SliceEngine.hpp"
#include "TestEngineRun.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

EngineRun run_on(const std::string& input_file, const char* tag)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = input_file;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = std::string("/tmp/orca_test_tm_") + tag;

    EngineRun r;
    r.engine = std::make_unique<SliceEngine>(cfg, r.temp_files);
    r.engine->run();
    return r;
}

bool has_error_issue(const SliceOutputStats& s, const std::string& code)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == IssueLevel::error)
            return true;
    return false;
}

// Engine-side plate_id is 0-based (JsonReport adds 1 for the JSON schema).
std::vector<int> error_issue_plate_ids(const SliceOutputStats& s, const std::string& code)
{
    std::vector<int> out;
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == IssueLevel::error)
            out.push_back(iss.plate_id);
    return out;
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

// --- [red] Object-level feature-filament keys (desktop Plater source ②). ----
// Common baseline: 2 slots (1=Generic PLA low-temp, 2=Generic ABS high-temp),
// ALL global feature keys = slot 1, object extruder explicitly = 1. The only
// ABS reference is the key under test, stored on <object> in
// model_settings.config. Today the engine never reads it -> slices fine.

TEST_CASE("RED: object-level sparse-infill ABS + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("obj_feature_sparse_infill_abs_fail.3mf"), "obj_sp");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("RED: object-level wall ABS + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("obj_feature_wall_abs_fail.3mf"), "obj_wl");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("RED: object-level solid-infill ABS + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("obj_feature_solid_infill_abs_fail.3mf"), "obj_so");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

// --- [red] Part-level feature-filament keys (desktop Plater source ③). ------
// Same baseline, but the ABS-referencing key sits on a <part> (a MODEL_PART
// volume). Desktop collects every volume's config; the engine only calls
// get_extruders() on model parts, which never reads feature keys.

TEST_CASE("RED: part-level sparse-infill ABS + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("part_feature_sparse_infill_abs_fail.3mf"), "prt_sp");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("RED: part-level solid-infill ABS + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("part_feature_solid_infill_abs_fail.3mf"), "prt_so");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("RED: part-level wall ABS + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("part_feature_wall_abs_fail.3mf"), "prt_wl");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

// --- [red] Modifier volume (desktop Plater source ③, no is_model_part ------
// filter). The modifier carries extruder=2: its overlapping region prints in
// ABS while the body is PLA. The engine's collector skips non-model-part
// volumes entirely.

TEST_CASE("RED: modifier-volume ABS extruder + PLA body must fail temp mix",
          "[integration][tempmode][red]")
{
    auto e = run_on(fixture("modifier_extruder_abs_fail.3mf"), "mod_ex");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

// --- Guard: the fix must NOT over-collect. ----------------------------------
// A high-temp slot that is merely loaded but referenced by nothing must keep
// slicing (kills a "collect all num_filaments slots" regression), and a second
// slot of the same temperature class must not trip the high/low split.

TEST_CASE("Idle high-temp slot must not fail temp mix", "[integration][tempmode]")
{
    auto e = run_on(fixture("unused_abs_slot_ok.3mf"), "idle_abs");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    REQUIRE_FALSE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

TEST_CASE("Second same-temperature slot referenced by feature key must not fail",
          "[integration][tempmode]")
{
    auto e = run_on(fixture("obj_feature_same_temp_ok.3mf"), "same_tp");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    REQUIRE_FALSE(has_error_issue(e->stats(), "FILAMENT_TEMP_MIXING"));
}

// --- Guard: per-plate attribution must survive the fix. ---------------------
// Plate 1 carries an ABS-extruder object next to PLA ones (already detected
// today via the model-part extruder path); plate 2 stays clean. Whatever the
// collector grows to read, only plate 1 may be flagged.

TEST_CASE("Mixing blocks only the offending plate", "[integration][tempmode]")
{
    auto e = run_on(fixture("multi_plate_obj_extruder_fail.3mf"), "mp_mix");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    auto plates = error_issue_plate_ids(e->stats(), "FILAMENT_TEMP_MIXING");
    REQUIRE(plates.size() == 1);
    REQUIRE(plates[0] == 0);  // engine-side 0-based -> JSON plate_id 1
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
