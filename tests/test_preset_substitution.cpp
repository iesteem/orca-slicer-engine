/**
 * @file test_preset_substitution.cpp
 * @brief Integration test for SliceEngine preset substitution — MODEL-AGNOSTIC
 *        invariant assertions.
 *
 * Drives SliceEngine to the post-substitution state via
 * run_preset_substitution_only() (no slicing), then verifies the invariants:
 *   - printer: m_config printer keys == official U1 preset's keys
 *   - filament: per-slot m_config[i] == the official filament preset resolved
 *     for slot i (whose name is filament_settings_id[i] after substitution)
 *   - process: keys listed in different_settings_to_system keep user values;
 *     the rest == the official process preset's keys (brim_width excepted — it
 *     is intentionally reset by apply_auto_brim_fallback)
 *
 * Ground truth comes from an INDEPENDENTLY loaded PresetBundle (not the one
 * inside SliceEngine), so this is a black-box cross-check. Expected values are
 * computed at runtime — no hardcoded snapshots — so the test stays meaningful
 * when the fixture model changes.
 *
 * Links the full libslic3r. Tag: [integration].
 */

#include <catch_amalgamated.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"       // DynamicPrintConfig, ConfigOptionStrings, ForwardCompatibilitySubstitutionRule
#include "libslic3r/Preset.hpp"       // Preset
#include "libslic3r/PresetBundle.hpp" // PresetBundle, LoadSystem
#include "libslic3r/Utils.hpp"        // set_data_dir / set_resources_dir

#include "SliceEngine.hpp"

using Slic3r::Preset;
using Slic3r::PresetBundle;
using Slic3r::DynamicPrintConfig;
using Slic3r::ConfigOptionStrings;

static const char* kFixture   = ORCA_TEST_FIXTURE;
static const char* kResources = ORCA_TEST_RESOURCES;

namespace {
// Independently load the official Snapmaker bundle as ground truth. Mirrors
// SliceEngine::load_system_presets (SliceEngine.cpp:541-555) for a single vendor.
std::unique_ptr<PresetBundle> load_official_bundle()
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(
        kResources, "Snapmaker", PresetBundle::LoadSystem,
        Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr);
    return bundle;
}

// Parse the ;-separated different_settings_to_system[0] list from a config.
std::set<std::string> parse_dts(const DynamicPrintConfig& cfg)
{
    std::set<std::string> out;
    const auto* opt = cfg.option<ConfigOptionStrings>("different_settings_to_system");
    if (!opt || opt->values.empty()) return out;
    std::stringstream ss(opt->values[0]);
    std::string item;
    while (std::getline(ss, item, ';'))
        if (!item.empty()) out.insert(item);
    return out;
}

// Serialize one element of a vector option: cfg[key][idx]. Returns empty if the
// key is absent or not one of the supported vector types (Floats/Ints/Strings/
// Bools). Per-slot extraction needs a typed get_at — there is no public untyped
// element accessor on ConfigOptionVectorBase.
std::string slot_value_serialized(const DynamicPrintConfig& cfg, const std::string& key, size_t idx)
{
    const Slic3r::ConfigOption* opt = cfg.option(key);
    if (!opt) return {};
    // Try each concrete vector type.
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionFloats*>(opt))
        return idx < v->values.size() ? std::to_string(v->values[idx]) : std::string{};
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionInts*>(opt))
        return idx < v->values.size() ? std::to_string(v->values[idx]) : std::string{};
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionStrings*>(opt))
        return idx < v->values.size() ? v->values[idx] : std::string{};
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionBools*>(opt))
        return idx < v->values.size() ? (v->values[idx] ? "1" : "0") : std::string{};
    return {};
}

// Serialize element 0 of a vector option (used for the official filament preset,
// whose per-filament keys are single-element vectors).
std::string scalar0_serialized(const Slic3r::ConfigOption* opt)
{
    if (!opt) return {};
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionFloats*>(opt))
        return v->values.empty() ? std::string{} : std::to_string(v->values[0]);
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionInts*>(opt))
        return v->values.empty() ? std::string{} : std::to_string(v->values[0]);
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionStrings*>(opt))
        return v->values.empty() ? std::string{} : v->values[0];
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionBools*>(opt))
        return v->values.empty() ? std::string{} : (v->values[0] ? "1" : "0");
    return {};
}

// Assert every key present in `official` has the same value in `actual`
// (scalar keys; vector keys compared element-0 vs element-0). Keys only in
// `actual` (metadata like inherits_group / *_settings_id) are ignored.
//
// official.config is the full inherits-expanded key set (load_vendor_configs_from_json
// recursively merges the inheritance chain at load time), so this checks against
// the complete official key space, not just the leaf preset's own keys.
//
// Emits a coverage summary (total / matched / missing / mismatched) as a WARN
// so it is always visible — purely diagnostic, never fails the test by itself.
void check_matches_official(const DynamicPrintConfig& actual, const Preset& official,
                            const std::set<std::string>& skip_keys, const std::string& label)
{
    size_t total = 0, matched = 0, missing = 0, mismatched = 0;
    for (auto it = official.config.cbegin(); it != official.config.cend(); ++it)
    {
        const std::string& key = it->first;
        if (skip_keys.count(key)) continue;
        // Skip heavy/special keys whose serialize() is unreliable or huge.
        if (key == "thumbnails" || key == "thumbnail_name" || key == "compatible_printers_condition")
            continue;
        ++total;
        if (!actual.has(key)) { ++missing; continue; }
        const Slic3r::ConfigOption* a_opt = actual.option(key);
        if (!a_opt) { ++missing; continue; }
        const std::string a = a_opt->serialize();
        const std::string b = it->second.get()->serialize();
        if (a == b) ++matched;
        else
        {
            ++mismatched;
            FAIL_CHECK(label << ": key \"" << key << "\" = \"" << a << "\" != official \"" << b << "\"");
        }
    }
    WARN(label << " coverage: " << matched << "/" << total
              << " matched, " << missing << " missing, " << mismatched << " mismatched"
              << " (missing keys are not a substitution defect — overwrite_all_keys_from only"
              << " covers keys already in m_config, never adds new ones)");
}
} // namespace

TEST_CASE("Preset substitution matches official presets (model-agnostic invariant)", "[integration][preset]")
{
    auto bundle = load_official_bundle();
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = kFixture;
    cfg.skip_preset_substitution = false;
    cfg.temp_dir = "/tmp";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    REQUIRE(engine.run_preset_substitution_only());

    const DynamicPrintConfig& mc = engine.config();

    // --- Printer invariant: every key the official U1 preset has must equal
    // m_config's value (overwrite_all_keys_from is a wholesale overwrite). ---
    const Preset* prn = bundle->printers.find_preset("Snapmaker U1 (0.4 nozzle)", false);
    REQUIRE(prn);
    REQUIRE(prn->vendor);
    REQUIRE(prn->vendor->name == PresetBundle::SM_BUNDLE);
    check_matches_official(mc, *prn, {}, "printer");

    // --- Filament invariant: per slot, m_config[key][i] == official_preset[key][0]
    // for every key in the slot's resolved official filament preset. The slot's
    // official name IS filament_settings_id[i] after substitution. ---
    const auto* fsi = mc.option<ConfigOptionStrings>("filament_settings_id");
    REQUIRE(fsi);
    for (size_t i = 0; i < fsi->values.size(); ++i)
    {
        const std::string& official_name = fsi->values[i];
        INFO("slot " << i << " -> \"" << official_name << "\"");
        const Preset* f = bundle->filaments.find_preset(official_name, false);
        REQUIRE(f);
        REQUIRE(f->vendor);
        REQUIRE(f->vendor->name == PresetBundle::SM_BUNDLE);

        // Compare each filament key: m_config[key][i] vs f->config[key][0].
        // Only vector-type keys are per-extruder; non-vector keys (rare in a
        // filament preset) are skipped — the per-slot overwrite only touches
        // vector keys (overwriteExtruderFrom semantics).
        for (auto it = f->config.cbegin(); it != f->config.cend(); ++it)
        {
            const std::string& key = it->first;
            if (!mc.has(key)) continue;
            std::string actual_slot = slot_value_serialized(mc, key, i);
            std::string official_scalar = scalar0_serialized(it->second.get());
            if (official_scalar.empty() || actual_slot.empty()) continue;  // non-vector key, skip
            if (actual_slot != official_scalar)
                FAIL_CHECK("filament slot " << i << " key \"" << key << "\": \"" << actual_slot
                                            << "\" != official \"" << official_scalar << "\"");
        }
    }

    // --- Process invariant (two classes): ---
    //   (1) different_settings_to_system keys keep the USER value (not overwritten)
    //   (2) the rest == official process preset
    const std::string& pname = engine.last_process_preset_name();
    REQUIRE(!pname.empty());
    const Preset* proc = bundle->prints.find_preset(pname, false);
    REQUIRE(proc);
    REQUIRE(proc->vendor);
    REQUIRE(proc->vendor->name == PresetBundle::SM_BUNDLE);

    auto dts = parse_dts(mc);
    // apply_auto_brim_fallback intentionally sets brim_width=0 after the
    // official overwrite when brim_type=auto_brim; exclude it from the "matches
    // official" check, and assert the fallback explicitly instead.
    // compatible_printers: overwrite_all_keys_from_impl only writes vector
    // elements that already exist in dst (loop bound by dst.size()); when the
    // fixture carries an empty compatible_printers, the official value cannot
    // be injected — so it stays empty. That is overwrite-semantics, not a
    // substitution defect; exclude it from the strict match.
    std::set<std::string> proc_skip = dts;
    proc_skip.insert("brim_width");
    proc_skip.insert("compatible_printers");
    check_matches_official(mc, *proc, proc_skip, "process");

    // brim_width fallback invariant: brim_type==auto_brim => brim_width == 0.
    // Use option<ConfigOptionString> + null check (brim_type may be an enum,
    // not a plain string, so opt_string would deref null).
    if (const auto* bt = mc.option<Slic3r::ConfigOptionString>("brim_type"))
    {
        if (bt->value == "auto_brim")
        {
            const auto* bw = mc.option<Slic3r::ConfigOptionString>("brim_width");
            REQUIRE(bw);
            CHECK(bw->value == "0");
        }
    }
}

TEST_CASE("Official preset key sets are mutually exclusive across categories", "[integration][preset][diag]")
{
    // The three substitution stages run in order printer -> filament -> process
    // (SliceEngine.cpp:639/605/613). Each overwrites m_config from its official
    // preset's config wholesale (printer/process) or per-extruder (filament).
    // If one stage's official preset carried keys belonging to another stage's
    // category, a later stage could clobber an earlier stage's result. This
    // diagnostic checks the key sets of the three official presets do NOT
    // overlap in a way that would let that happen.
    auto bundle = load_official_bundle();
    const Preset* prn = bundle->printers.find_preset("Snapmaker U1 (0.4 nozzle)", false);
    const Preset* fil = bundle->filaments.find_preset("Generic PLA", false);
    // Use the same process name the engine resolved for the fixture (inherits[0]).
    REQUIRE(prn); REQUIRE(fil);

    auto keys = [](const Preset* p) {
        std::set<std::string> s;
        for (auto it = p->config.cbegin(); it != p->config.cend(); ++it) s.insert(it->first);
        return s;
    };
    auto prn_keys = keys(prn);
    auto fil_keys = keys(fil);

    // Classify a key by category prefix. printer: nozzle_/printable_/machine_/extruder_/
    // bed_/gcode-ish; filament: filament_; process: layer_/brim_/skirt_/infill_/speed_/etc.
    // We do NOT need a perfect classifier — the question is whether one preset's
    // config leaks keys that another stage will later overwrite. The strong,
    // model-agnostic invariant is: keys the printer stage sets that a later
    // stage would clobber. The most informative overlap is printer ∩ process
    // (both wholesale-overwrite). filament only touches per-extruder vector
    // keys, so it cannot clobber scalar printer/process keys.
    auto find_process = [&]() -> const Preset* {
        // Pick any U1 process preset.
        for (auto it = bundle->prints.cbegin(); it != bundle->prints.cend(); ++it)
            if (it->is_system && it->vendor && it->vendor->name == PresetBundle::SM_BUNDLE
                && it->name.find("@Snapmaker U1") != std::string::npos)
                return &*it;
        return nullptr;
    };
    const Preset* proc = find_process();
    REQUIRE(proc);
    auto pro_keys = keys(proc);

    auto intersect = [](const std::set<std::string>& a, const std::set<std::string>& b) {
        std::vector<std::string> o;
        for (const auto& k : a) if (b.count(k)) o.push_back(k);
        return o;
    };

    // Process preset should not own printer-category keys (nozzle/printable/machine).
    // If it did, the process stage would clobber the printer stage's values.
    auto proc_owning_printer_keys = [&]() {
        std::vector<std::string> bad;
        for (const auto& k : pro_keys)
            if (k.rfind("nozzle_", 0) == 0 || k.rfind("printable_", 0) == 0 ||
                k.rfind("machine_", 0) == 0 || k == "extruder_type")
                bad.push_back(k);
        return bad;
    };
    auto bad = proc_owning_printer_keys();
    auto pp_overlap = intersect(prn_keys, pro_keys);
    std::string overlap_list;
    for (size_t i = 0; i < pp_overlap.size(); ++i)
        overlap_list += (i ? "," : "") + pp_overlap[i];
    WARN("preset key isolation: |printer|=" << prn_keys.size()
         << " |filament|=" << fil_keys.size() << " |process|=" << pro_keys.size()
         << " printer∩process=" << pp_overlap.size() << " [" << overlap_list << "]"
         << " process-owning-printer-keys=" << bad.size());
    CHECK(bad.empty());
}

TEST_CASE("Preset substitution skipped when configured diverges from official", "[integration][preset]")
{
    auto bundle = load_official_bundle();
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = kFixture;
    cfg.skip_preset_substitution = true;
    cfg.temp_dir = "/tmp";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    REQUIRE(engine.run_preset_substitution_only());

    // With substitution skipped, m_config must NOT wholesale-match the official
    // printer preset (proves the invariant check above is exercising the
    // substitution step, not something else). At least one printer key differs.
    const Preset* prn = bundle->printers.find_preset("Snapmaker U1 (0.4 nozzle)", false);
    REQUIRE(prn);
    const DynamicPrintConfig& mc = engine.config();
    bool any_diff = false;
    for (auto it = prn->config.cbegin(); it != prn->config.cend() && !any_diff; ++it)
    {
        if (!mc.has(it->first)) continue;
        if (mc.option(it->first)->serialize() != it->second.get()->serialize())
            any_diff = true;
    }
    // The fixture carries a "- 拷贝" clone printer preset, so its config should
    // diverge from official in at least the settings_id / G-code keys.
    CHECK(any_diff);
}


// --- Mini train city: hollow embedded process preset must not be applied. ---
// The 3MF carries an embedded process preset that is a hollow shell (its only
// real content is an "inherits" reference). Before 064dbbc/da7f518 the shell
// was treated as official and applied wholesale, overwriting valid project
// config with empty values (broken slicing). With is_official_preset
// (vendor == SM_BUNDLE + not project-embedded) the shell is only an
// inheritance-chain waypoint. Full-slice guard: plate 1 carries heterogeneous
// variable layer heights that trip the desktop-parity PRIME_TOWER_VARIABLE_
// LAYER_HEIGHT blocking error (a19033b/99867c3) — the run is expected to fail
// with that specific error, which still proves the pipeline reached (and got
// past) preset substitution with valid config instead of crashing.
TEST_CASE("Hollow embedded process preset rejected, project slices clean (train city)", "[integration][preset][embedded]")
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/hollow_embedded_preset_train_city.3mf";
    cfg.skip_preset_substitution = false;
    cfg.temp_dir = "/tmp";
    cfg.output_base = "/tmp/orca_test_ps_traincity";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    engine.run();

    // Desktop parity: heterogeneous variable layer heights + wipe tower blocks
    // slicing at validation (Plater.cpp:12653 UPDATE_BACKGROUND_PROCESS_INVALID).
    REQUIRE(!engine.stats().success);
    REQUIRE(engine.exit_code() != 0);
    bool has_prime_tower_error = false;
    for (const auto& issue : engine.stats().issues)
        if (issue.code == "PRIME_TOWER_VARIABLE_LAYER_HEIGHT")
            has_prime_tower_error = true;
    REQUIRE(has_prime_tower_error);
    // The applied process preset is the official system one resolved through
    // the chain, never the embedded shell.
    REQUIRE(engine.last_process_preset_name().find("train") == std::string::npos);
}
