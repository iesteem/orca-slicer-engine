/**
 * @file test_print_sequence.cpp
 * @brief Integration test for print-sequence (by-layer vs by-object) handling.
 *
 * The U1 toolchanger cannot print by object safely — tool switches risk
 * colliding with printed parts. SliceEngine::check_print_by_object()
 * (src/SliceEngine.cpp) blocks any plate whose effective print_sequence
 * resolves to ByObject, after per-plate config overrides are applied on top
 * of the global (project-level) setting.
 *
 * Three outcome classes (measured 2026-09-09, matching fixture filenames):
 *   - Global by-object + single plate overriding to by-layer: the per-plate
 *     override wins, slicing succeeds: exit 0.
 *   - Global by-layer + single plate declaring by-object: blocked at
 *     validation, PRINT_BY_OBJECT_CAUTION error, exit 6 (preprocess).
 *   - Two plates, one blocked and one by-layer: the by-layer plate slices
 *     but the job fails overall (all-or-nothing packaging), exit 6.
 * Tag: [integration][seq].
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
    cfg.output_base = std::string("/tmp/orca_test_seq_") + tag;

    EngineRun r;
    r.engine = std::make_unique<SliceEngine>(cfg, r.temp_files);
    r.engine->run();
    return r;
}

bool has_error_issue(const SliceOutputStats& s, const std::string& code, int plate_id = -1)
{
    for (const auto& iss : s.issues)
        if (iss.code == code && iss.level == IssueLevel::error
            && (plate_id < 0 || iss.plate_id == plate_id))
            return true;
    return false;
}

std::string fixture(const char* name)
{
    return std::string(ORCA_TEST_FIXTURE_DIR) + "/print_sequence/" + name;
}
} // namespace

// --- Success: per-plate by-layer override beats global by-object. ----------

TEST_CASE("Global by-object with plate by-layer override slices OK", "[integration][seq]")
{
    auto e = run_on(fixture("global_by_object_plate_by_layer_ok.3mf"), "gobj_pl");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    REQUIRE_FALSE(has_error_issue(e->stats(), "PRINT_BY_OBJECT_CAUTION"));
}

// --- Failure: plate declares by-object under global by-layer. --------------

TEST_CASE("Plate by-object under global by-layer is blocked", "[integration][seq]")
{
    auto e = run_on(fixture("global_by_layer_plate_by_object_fail.3mf"), "glyr_pobj");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->any_error());
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    // Internal plate ids are 0-based (JSON export adds 1).
    REQUIRE(has_error_issue(e->stats(), "PRINT_BY_OBJECT_CAUTION", /*plate_id=*/0));
}

// --- Partial failure: one blocked plate fails the whole job. ---------------

TEST_CASE("Mixed plates: by-object plate blocks, by-layer plate slices", "[integration][seq]")
{
    auto e = run_on(fixture("mixed_plates_partial_fail.3mf"), "mixed");
    REQUIRE_FALSE(e->stats().success);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(has_error_issue(e->stats(), "PRINT_BY_OBJECT_CAUTION", /*plate_id=*/0));
    // Plate 2 (by-layer, internal id 1) still slices; only plate 1 is blocked.
    REQUIRE(e->stats().plates.size() == 2);
    REQUIRE_FALSE(e->stats().plates[0].success);
    REQUIRE(e->stats().plates[1].success);
}
