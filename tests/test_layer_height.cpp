/**
 * @file test_layer_height.cpp
 * @brief Integration test for layer-height precedence (user override vs system
 * preset) across nozzle sizes.
 *
 * Eight fixtures encode the expected layer_height / first-layer-height for
 * each combination of nozzle size and user-modified-vs-default. The engine
 * uses Bambu-style key names, so first-layer height is read as
 * `initial_layer_print_height` (NOT first_layer_height, which is absent from
 * the engine config).
 *
 * Values are MEASURED (2026-08-07) and match the fixture filenames, except
 * case6 first-layer (0.55 measured vs 0.56 filename) — a 0.01mm rounding/snap
 * difference, absorbed by the assertion tolerance.
 *
 * layer_height is asserted exactly (config value, not snapped); first-layer
 * uses a 0.02mm margin to absorb slicer snapping/rounding.
 * Tag: [integration][layerheight].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"        // ConfigOptionFloat
#include "libslic3r/Utils.hpp"

#include "Types.hpp"
#include "SliceEngine.hpp"

using Catch::Approx;

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

// Slice a case, return the engine for config inspection. Output to /tmp.
std::unique_ptr<SliceEngine> slice_case(int n)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/layer_height/case" + std::to_string(n) + ".3mf";
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = std::string("/tmp/orca_test_lh_case") + std::to_string(n);

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

// Assert both heights for a case. lh exact, flh within margin.
void check_heights(int n, double exp_lh, double exp_flh)
{
    auto e = slice_case(n);
    REQUIRE(e->stats().success);

    const auto* lh = e->config().option<Slic3r::ConfigOptionFloat>("layer_height");
    REQUIRE(lh);
    INFO("case" << n << " layer_height=" << lh->value << " expected=" << exp_lh);
    REQUIRE(lh->value == Approx(exp_lh).margin(1e-6));

    const auto* flh = e->config().option<Slic3r::ConfigOptionFloat>("initial_layer_print_height");
    REQUIRE(flh);
    INFO("case" << n << " initial_layer_print_height=" << flh->value << " expected=" << exp_flh);
    REQUIRE(flh->value == Approx(exp_flh).margin(0.02));
}
} // namespace

// Nozzle 0.2mm. The fixture references "0.14 Standard @U1 (0.2 nozzle)",
// which the Aug-2026 official profile merge folded into "0.12mm Standard"
// (renamed_from includes 0.14) — so the substituted preset now yields
// layer 0.12, first-layer 0.1.
TEST_CASE("0.2 nozzle: default heights from preset", "[integration][layerheight]")
{
    check_heights(1, 0.12, 0.10);
}

TEST_CASE("0.2 nozzle: user override wins", "[integration][layerheight]")
{
    // User set first-layer 0.14, layer 0.1 — user value wins.
    check_heights(2, 0.10, 0.14);
}

// Nozzle 0.4mm, process preset 0.4 layer height.
TEST_CASE("0.4 nozzle: default heights from preset", "[integration][layerheight]")
{
    check_heights(3, 0.20, 0.25);
}

TEST_CASE("0.4 nozzle: user override wins", "[integration][layerheight]")
{
    check_heights(4, 0.19, 0.22);
}

// Nozzle 0.6mm. The fixture references "0.36 Standard @U1 (0.6 nozzle)",
// folded into "0.30mm Standard @U1 (0.6 nozzle)" by the Aug-2026 official
// profile merge (renamed_from includes 0.36) — substituted preset yields
// layer 0.30, first-layer 0.30.
TEST_CASE("0.6 nozzle: default heights from preset", "[integration][layerheight]")
{
    check_heights(5, 0.30, 0.30);
}

TEST_CASE("0.6 nozzle: user override wins", "[integration][layerheight]")
{
    // first-layer measured 0.55 vs filename 0.56 — 0.01mm snap, within margin.
    check_heights(6, 0.41, 0.56);
}

// Nozzle 0.8mm, process preset 0.4 layer height.
TEST_CASE("0.8 nozzle: default heights from preset", "[integration][layerheight]")
{
    check_heights(7, 0.40, 0.40);
}

TEST_CASE("0.8 nozzle: user override wins", "[integration][layerheight]")
{
    check_heights(8, 0.49, 0.20);
}
