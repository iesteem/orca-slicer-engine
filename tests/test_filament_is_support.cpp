/**
 * @file test_filament_is_support.cpp
 * @brief Integration test for the plates[].filaments[].is_support declaration.
 *
 * The App one-key print flow consumes the engine JSON passthrough and cannot
 * see the deployed filament presets, so the engine must report the per-slot
 * support-material declaration itself (2.2.1 contract, json-output.md).
 *
 * Declaration-layer semantics (same as volume_type): the post-apply
 * print.config() value — the preset declaration, NOT evidence that support
 * was extruded (that is support_free, gcode actual layer; the two are
 * deliberately different sources).
 *
 * Fixture: fixtures/support/support_filament_per_plate.3mf — cloned from
 * filement_count/distinct_per_plate.3mf (4 plates, each using exactly one
 * slot — pinned by test_filament_count) with slot 2's preset swapped to the
 * official "Generic Support For PLA @U1 0.4 nozzle" (filament_is_support=1).
 * Plate 2 -> slot 2 = support; plates 1/3/4 -> non-support.
 *
 * Tag: [integration][issupport].
 */

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Utils.hpp" // set_data_dir / set_resources_dir

#include "JsonReport.hpp"
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
    cfg.output_base = std::string("/tmp/orca_test_issup_") + tag;

    EngineRun r;
    r.engine = std::make_unique<SliceEngine>(cfg, r.temp_files);
    r.engine->run();
    return r;
}

// Read one text entry (e.g. "Metadata/plate_2.gcode") from the packaged
// .gcode.3mf. libzip is not linked into the test binary; shell out to
// python3 (same rationale as extract_3mf_text in test_high_flow.cpp) and
// fail loudly if either step breaks.
std::string extract_3mf_text(const std::string& gcode_3mf_path, const std::string& entry)
{
    const std::string tmp = "/tmp/orca_test_issup_entry.txt";
    boost::filesystem::remove(tmp);
    const std::string cmd =
        "python3 -c \"import zipfile,sys;"
        "z=zipfile.ZipFile('" + gcode_3mf_path + "');"
        "sys.exit(1) if '" + entry + "' not in z.namelist() "
        "else open('" + tmp + "','wb').write(z.read('" + entry + "'))\"";
    const int rc = std::system(cmd.c_str());
    REQUIRE(rc == 0);
    std::ifstream f(tmp, std::ios::binary);
    REQUIRE(f.good());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("filaments[].is_support reports the per-slot support-material declaration",
          "[integration][issupport]")
{
    // Stale-artifact relocation trap — pre-clean the output path (an existing
    // file there silently relocates the output to a timestamped name).
    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_issup_decl.gcode.3mf", ec);

    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/support/support_filament_per_plate.3mf",
                    "decl");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);
    REQUIRE(e->stats().plates.size() == 4);

    // Structural layer: one extruded slot per plate; only plate 2 (slot 2,
    // 0-based extruder 1) carries the support declaration.
    for (int p = 0; p < 4; ++p)
    {
        REQUIRE(e->stats().plates[p].filament_details.size() == 1);
        CHECK(e->stats().plates[p].filament_details.at(0).is_support == (p == 1));
    }

    // Emission layer: the report carries the field as a JSON bool, placed in
    // plates[].filaments[] with the same per-slot values.
    const auto j = nlohmann::json::parse(
        build_statistics_json(e->stats(), "/tmp/orca_test_issup_decl.gcode.3mf"));
    for (int p = 0; p < 4; ++p)
    {
        const auto& fj = j["plates"].at(p)["filaments"].at(0);
        CHECK(fj.contains("is_support"));
        CHECK(fj["is_support"].is_boolean());
        CHECK(fj["is_support"].get<bool>() == (p == 1));
    }

    // Same-source check: the "; filament_is_support" gcode header (echoed from
    // the same post-apply print.config() the JSON snapshot reads) must agree
    // with the reported declaration — slot 2 set, the rest clear.
    const std::string gcode = extract_3mf_text("/tmp/orca_test_issup_decl.gcode.3mf",
                                               "Metadata/plate_2.gcode");
    CHECK(gcode.find("; filament_is_support = 0,1,0,0") != std::string::npos);

    boost::filesystem::remove("/tmp/orca_test_issup_decl.gcode.3mf", ec);
}

TEST_CASE("filaments[].is_support collapses to false when no support filament is declared",
          "[integration][issupport]")
{
    // geom_above_bed_ok.3mf declares plain filaments only: the join must
    // render false (the collapse default) for the used slot. Pins the
    // missing/default -> false contract the App one-key print flow relies on
    // (the App-side fallback and the engine-side collapse must agree).
    boost::system::error_code ec;
    boost::filesystem::remove("/tmp/orca_test_issup_std.gcode.3mf", ec);

    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/geom_above_bed_ok.3mf", "std");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);

    const auto& plate = e->stats().plates.at(0);
    REQUIRE(plate.filament_details.size() == 1);
    CHECK(plate.filament_details.at(0).is_support == false);

    const auto j = nlohmann::json::parse(
        build_statistics_json(e->stats(), "/tmp/orca_test_issup_std.gcode.3mf"));
    CHECK(j["plates"].at(0)["filaments"].at(0)["is_support"] == false);

    boost::filesystem::remove("/tmp/orca_test_issup_std.gcode.3mf", ec);
}
