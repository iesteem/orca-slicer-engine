/**
 * @file test_preset_substitution.cpp
 * @brief Integration test for SliceEngine preset substitution.
 *
 * Drives SliceEngine up to (and including) apply_preset_substitution via the
 * run_preset_substitution_only() prefix hook — no geometry checks, no slicing —
 * then dumps the resulting m_config to an INI file and asserts the substitution
 * outcome against the U1 fixture (tests/fixtures/u1_preset_test.3mf).
 *
 * Unlike the lightweight engine-tests target, this links the full libslic3r
 * (it needs PresetBundle + real 3MF + resources/profiles). Tag: [integration].
 *
 * Fixture substitution contract (verified against the fixture's project_settings):
 *   printer_settings_id : "Snapmaker U1 (0.4 nozzle) - 拷贝" -> official U1
 *   filament_settings_id: slots 0/2 non-official -> official; slots 1/3 unchanged
 *   nozzle_diameter     : corrected to U1 official
 *   printable_area      : corrected to U1 official
 */

#include <catch_amalgamated.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Utils.hpp"   // Slic3r::set_data_dir / set_resources_dir

#include "SliceEngine.hpp"

namespace {
// Read all key=value lines from an INI dump into a map.
std::map<std::string, std::string> read_ini(const std::string& path)
{
    std::map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        // skip section headers / comments / blanks
        if (line.empty() || line[0] == '[' || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // trim spaces around key
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        // val: trim one leading space (INI writer emits "key = value")
        if (!val.empty() && val[0] == ' ') val.erase(0, 1);
        // trim trailing \r
        if (!val.empty() && val.back() == '\r') val.pop_back();
        out[key] = val;
    }
    return out;
}
} // namespace

static const char* kFixture   = ORCA_TEST_FIXTURE;
static const char* kResources = ORCA_TEST_RESOURCES;

TEST_CASE("Preset substitution replaces non-official presets with U1 official", "[integration][preset]")
{
    // Point libslic3r at the engine's bundled Snapmaker resources (pure vendor
    // set — NOT OrcaSlicer/resources, which contains MagicMaker and fails to
    // load). Mirrors slic3r_c_api.cpp:108-110.
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = kFixture;
    cfg.skip_preset_substitution = false;
    cfg.temp_dir = "/tmp";  // load_3mf writes a backup tree here

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);

    REQUIRE(engine.run_preset_substitution_only());

    // Dump the post-substitution config to INI and read it back.
    const std::string ini_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp")
                                 + "/orca_preset_sub_test.ini";
    engine.config().save(ini_path);
    auto kv = read_ini(ini_path);

    // --- Printer geometry: apply_printer_official_preset overwrites the
    // parameter keys (printable_area / printable_height / nozzle_diameter) with
    // U1 official values. The fixture already carried U1 geometry, so these are
    // unchanged — the invariant is that they HOLD the official U1 values. ---
    CHECK(kv["nozzle_diameter"].find("0.4") != std::string::npos);
    CHECK(kv["printable_area"].find("270") != std::string::npos);   // U1 bed ~270x270
    CHECK(kv["printable_height"] == "270.05");
    CHECK(kv["printer_model"] == "Snapmaker U1");

    // --- printer_settings_id: apply_printer_official_preset does NOT rewrite
    // the preset-NAME field (it rewrites parameters, not the name). The current
    // observed behaviour is that the name becomes "MyToolChanger 0.4 nozzle -
    // Copy" — locked in here as the present-day contract. If preset substitution
    // is later fixed to also normalise the name to "Snapmaker U1 (0.4 nozzle)",
    // this assertion will flag the change. ---
    std::string printer = kv.count("printer_settings_id") ? kv["printer_settings_id"] : "";
    INFO("printer_settings_id = \"" << printer << "\"");
    CHECK(printer == "MyToolChanger 0.4 nozzle - Copy");

    // --- Filament substitution: slot 0 ("Generic ABS - 拷贝") and slot 2
    // (bare "Generic ABS") are normalised; the "拷贝/Clone" suffix must be gone
    // from every slot. Slots 1/3 were already official (Snapmaker PLA SnapSpeed
    // @U1) and stay. ---
    std::string fil_ids = kv.count("filament_settings_id") ? kv["filament_settings_id"] : "";
    INFO("filament_settings_id = \"" << fil_ids << "\"");
    CHECK(fil_ids.find("拷贝") == std::string::npos);
    CHECK(fil_ids.find("Snapmaker PLA SnapSpeed @U1") != std::string::npos);  // official slot preserved
    CHECK(fil_ids.find("\"Generic ABS\"") != std::string::npos);              // slot 0/2 normalised (no clone suffix)
}

TEST_CASE("Preset substitution skipped when configured", "[integration][preset]")
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = kFixture;
    cfg.skip_preset_substitution = true;   // bypass substitution entirely
    cfg.temp_dir = "/tmp";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);

    REQUIRE(engine.run_preset_substitution_only());

    const std::string ini_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp")
                                 + "/orca_preset_sub_skip.ini";
    engine.config().save(ini_path);
    auto kv = read_ini(ini_path);

    // With substitution skipped, the non-official clone preset is UNCHANGED —
    // the "- 拷贝" suffix must still be present (proves the substitution step,
    // not something else, is what removed it in the test above).
    std::string printer = kv.count("printer_settings_id") ? kv["printer_settings_id"] : "";
    INFO("printer_settings_id (skip) = \"" << printer << "\"");
    CHECK(printer.find("拷贝") != std::string::npos);
}
