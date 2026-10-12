/**
 * @file test_preset_substitution.cpp
 * @brief Integration test for SliceEngine preset substitution — MODEL-AGNOSTIC
 *        invariant assertions.
 *
 * Drives SliceEngine to the post-substitution state via
 * run_preset_substitution_only() (no slicing), then verifies the invariants:
 *   - printer: m_config printer keys == official U1 preset's keys
 *   - filament: per-slot m_config[i] == the official filament preset resolved
 *     for slot i (whose name is filament_settings_id[i] after substitution);
 *     every per-filament vector must cover ALL slots — a vector shorter than
 *     filament_settings_id is the C3 short-side truncation defect
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
#include "libslic3r/PrintConfig.hpp"  // is_filament_flow_variant_option
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

// Length of a vector option (0 when absent or not a vector). Unlike the
// typed extractors above this works for EVERY vector option type via the
// untyped ConfigOptionVectorBase base, so it can assert slot coverage for
// keys the typed extractors cannot serialize (e.g. enum vectors).
size_t vector_opt_length(const DynamicPrintConfig& cfg, const std::string& key)
{
    const Slic3r::ConfigOption* opt = cfg.option(key);
    if (!opt) return 0;
    if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(opt))
        return v->size();
    return 0;
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
        const Slic3r::ConfigOption* actual_opt = actual.option(key);
        if (!actual_opt) { ++missing; continue; }
        std::string actual_str = actual_opt->serialize();
        std::string official_str = it->second.get()->serialize();
        // Aug-2026 high-flow data model: official presets store flow-variant
        // vectors (e.g. inner_wall_speed "300,600", printer_flow_support
        // "standard;high_flow"); the engine config holds the active
        // (standard) scalar. Compare against the first element when actual
        // is scalar.
        if (actual_str.find(',') == std::string::npos && actual_str.find(';') == std::string::npos)
        {
            const size_t sep = official_str.find_first_of(",;");
            if (sep != std::string::npos)
                official_str = official_str.substr(0, sep);
        }
        if (actual_str == official_str) ++matched;
        else
        {
            ++mismatched;
            FAIL_CHECK(label << ": key \"" << key << "\" = \"" << actual_str
                      << "\" != official \"" << official_str << "\"");
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

    // Fixture-drift sentinel (replaces the former skip-substitution counter-
    // test): the official U1 preset JSON itself carries the historical dirty
    // id "MyToolChanger 0.4 nozzle - Copy" (cloud-review root cause), so the
    // assertion compares against the preset's OWN value instead of a literal
    // name. If substitution ever stops running (or kFixture drifts to an
    // official re-export making wholesale-match vacuous), m_config keeps the
    // 3MF's original id — "Snapmaker U1 (0.4 nozzle) - 拷贝" — which cannot
    // equal the preset's dirty value, so this REQUIRE is the check that goes
    // red.
    const auto* psi = mc.option<Slic3r::ConfigOptionString>("printer_settings_id");
    REQUIRE(psi);
    const auto* off_psi = prn->config.option<Slic3r::ConfigOptionString>("printer_settings_id");
    REQUIRE(off_psi);
    REQUIRE(psi->value == off_psi->value);

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
        // vector keys (overwriteExtruderFrom semantics). Also skipped:
        // non-per-slot vectors (compatibility lists, flow declarations) and
        // flow-variant keys (segmented storage, never per-slot overwritten).
        // Every OTHER vector must cover ALL slots — a vector shorter than the
        // slot count is the C3 short-side truncation defect.
        static const std::set<std::string> kNonPerSlotKeys = {
            "compatible_prints", "compatible_printers",
            "compatible_prints_condition", "compatible_printers_condition",
            "default_filament_profile", "filament_flow_support",
            "filament_flow_step_size", "inherits"};
        for (auto it = f->config.cbegin(); it != f->config.cend(); ++it)
        {
            const std::string& key = it->first;
            if (!mc.has(key)) continue;
            const size_t official_len = vector_opt_length(f->config, key);
            const size_t actual_len   = vector_opt_length(mc, key);
            if (official_len == 0 && actual_len == 0) continue; // non-vector key, skip
            if (kNonPerSlotKeys.count(key)) continue;
            if (Slic3r::is_filament_flow_variant_option(key)) continue;
            if (actual_len <= i)
                FAIL_CHECK("filament slot " << i << " key \"" << key << "\": vector holds "
                                           << actual_len << " elements — short-side truncation");
            std::string actual_slot = slot_value_serialized(mc, key, i);
            std::string official_scalar = scalar0_serialized(it->second.get());
            if (official_scalar.empty() || actual_slot.empty()) continue; // unserializable type
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
    // brim_width no longer needs an exemption: since the 2026-09 auto-brim
    // write-back fix, m_config keeps the merged value (official process
    // preset's, unless the user overrode it via different_settings_to_system);
    // the auto-mode zero lives only on the per-plate slicing copy inside
    // prepare_merged_config_for_plate, and the G-code header echo is restored
    // after export (restore_brim_width_echo).
    // compatible_printers / small_area_infill_flow_compensation_model no
    // longer need exemptions either: since the 2026-09-20 C3 fix the vector
    // overwrite replaces dst options wholesale (desktop apply() semantics),
    // so empty-array keys receive the official (possibly empty) value verbatim
    // instead of silently keeping the author-side empty array.
    std::set<std::string> proc_skip = dts;
    // 2026-10-12 base-bump drift (upstream b626b4be62, Orca 2.4.1 sync):
    // top/bottom_color_penetration_layers are NEW PrintRegionConfig keys.
    // Projects saved before the key existed (this fixture) do not carry it,
    // and the substitution contract never ADDS keys the project lacks
    // (overwrite_all_keys_from only overwrites existing m_config entries) —
    // the config layer later fills the CODE default (top=4), while the
    // official U1 presets pin an explicit 5 (penetration <= shell layers;
    // fdm_process_U1_0.20.json: top_shell_layers=5). Desktop parity holds:
    // opening the same old project without re-selecting the preset also
    // leaves the code default active. These keys are therefore outside the
    // substitution invariant by the same logic as "missing keys are not a
    // substitution defect"; the exemption is keyed by family so a future
    // upstream default change that ACTUALLY diverges on a project-carried
    // key still fails this test.
    static const std::set<std::string> kCodeDefaultFilledKeys = {
        "top_color_penetration_layers",    // code default 4, official 5 — drift class documented above
        "bottom_color_penetration_layers", // same commit; code default 3 == official 3 today
    };
    proc_skip.insert(kCodeDefaultFilledKeys.begin(), kCodeDefaultFilledKeys.end());
    check_matches_official(mc, *proc, proc_skip, "process");

    // Auto-brim invariant (2026-09 fix): when brim_type=auto_brim, m_config's
    // brim_width equals the official process preset's value — it is NOT zeroed
    // on m_config anymore. Typed lookups matter here: brim_type is
    // ConfigOptionEnum<BrimType> and brim_width is ConfigOptionFloat; the
    // previous ConfigOptionString lookups always returned nullptr, so the old
    // "== 0" assertion was dead code that never executed.
    // The preconditions below are REQUIREs on purpose: this fixture
    // (u1_preset_test.3mf) is auto_brim, and the previous soft `if (bt)` guard
    // let the invariant die silently for six weeks when the lookup went null.
    const auto* bt = mc.option<Slic3r::ConfigOptionEnum<Slic3r::BrimType>>("brim_type");
    REQUIRE(bt);
    REQUIRE(bt->value == Slic3r::btAutoBrim);
    {
        const auto* bw = mc.option<Slic3r::ConfigOptionFloat>("brim_width");
        const auto* ow = proc->config.option<Slic3r::ConfigOptionFloat>("brim_width");
        REQUIRE(bw);
        REQUIRE(ow);
        INFO("brim_width m_config=" << bw->value << " official=" << ow->value);
        CHECK(bw->value == ow->value);
        CHECK(bw->value != 0.0);
    }
}

// C3 regression: the fixture declares 4 filament slots (filament_settings_id)
// but carries six per-filament vectors truncated to 2 entries. Pre-fix, the
// per-slot substitution loop still visits slots 2/3 but overwriteExtruderFrom
// silently skips them for every short vector (dst_vec->size() <= dst_idx ->
// continue), so raw 3MF user values survive substitution and flow into
// slicing. Post-fix, slot-count normalization grows every per-filament vector
// to 4 before the loop runs, so all slots are substituted.
TEST_CASE("Truncated per-filament vectors normalized to slot count", "[integration][preset]")
{
    auto bundle = load_official_bundle();
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/u1_preset_test_truncated.3mf";
    cfg.temp_dir = "/tmp";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    REQUIRE(engine.run_preset_substitution_only());

    const DynamicPrintConfig& mc = engine.config();

    const auto* fsi = mc.option<ConfigOptionStrings>("filament_settings_id");
    REQUIRE(fsi);
    REQUIRE(fsi->values.size() == 4);

    // The six truncated vectors must be normalized back to the slot count
    // (nozzle_temperature* are flow-variant keys: sizing is their fix, their
    // values are never per-slot overwritten).
    for (const char* key : {"filament_type", "filament_diameter", "filament_colour",
                            "nozzle_temperature", "nozzle_temperature_initial_layer",
                            "cool_plate_temp"})
    {
        INFO("key " << key);
        CHECK(vector_opt_length(mc, key) == 4);
    }

    // run_preset_substitution_only also runs normalize_loaded_config, so the
    // flush matrix must be re-derived from the restored 4-slot count.
    if (const auto* m = mc.option<Slic3r::ConfigOptionFloats>("flush_volumes_matrix"))
        CHECK(m->values.size() == 16);

    // Slots 0..3 of non-flow-variant keys that official filament presets
    // carry must equal the slot's official preset scalar0.
    for (size_t i = 0; i < fsi->values.size(); ++i)
    {
        const Preset* f = bundle->filaments.find_preset(fsi->values[i], false);
        REQUIRE(f);
        for (const char* key : {"filament_type", "filament_diameter", "cool_plate_temp"})
        {
            INFO("slot " << i << " key " << key << " -> \"" << fsi->values[i] << "\"");
            std::string actual   = slot_value_serialized(mc, key, i);
            std::string official = scalar0_serialized(f->config.option(key));
            CHECK_FALSE(actual.empty());
            if (!official.empty())
                CHECK(actual == official);
        }
    }
}

// Shrink-side mirror of the C3 case above (release-review blind spot
// 2026-10-10: the only un-anchored path of
// normalize_filament_vectors_to_slot_count). The fixture declares 4 slots but
// stretches five pass-1 core per-filament vectors to 6 entries — the shape a
// third-party exporter produces when it trims filament_settings_id without
// resizing the rest. Pass-1 exact-resizes core keys to the slot count (grow
// AND shrink); a vector left at 6 would flow mismatched dimensions into every
// downstream slot indexer (the mushroom ToolOrdering crash family).
TEST_CASE("Oversized per-filament vectors shrunk to slot count", "[integration][preset]")
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/u1_preset_test_oversized.3mf";
    cfg.temp_dir = "/tmp";

    std::vector<std::string> temp_files;
    SliceEngine engine(cfg, temp_files);
    REQUIRE(engine.run_preset_substitution_only());

    const DynamicPrintConfig& mc = engine.config();

    // The slot authority drives n and is never edited by normalization.
    const auto* fsi = mc.option<ConfigOptionStrings>("filament_settings_id");
    REQUIRE(fsi);
    REQUIRE(fsi->values.size() == 4);

    // The five stretched pass-1 core keys must be cut BACK to 4 (not left 6).
    for (const char* key : {"retraction_length", "z_hop", "wipe",
                            "filament_diameter", "filament_colour"})
    {
        INFO("key " << key);
        CHECK(vector_opt_length(mc, key) == 4);
    }

    // Flush matrix re-derived from the restored 4-slot count (same contract
    // as the C3 case; the fixture ships it 6x6 = 36 entries).
    if (const auto* m = mc.option<Slic3r::ConfigOptionFloats>("flush_volumes_matrix"))
        CHECK(m->values.size() == 16);
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

// (Removed 2026-09-29: "Preset substitution skipped when configured diverges
// from official" tested the --skip-preset-substitution flag, which was removed
// with the flag. Its meta-verification role — proving wholesale-match above
// exercises the substitution step — is carried by the fixture-drift sentinel
// in the invariant test: kFixture's clone preset id differs from official, so
// the id equality can only hold if substitution rewrote it.)


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
// (Note: on Orca feature-2.3.5-base the exception is untyped NOT_DEFINED and
// the classifier downgrades it to a warning — the 05 branch carried that
// variant. This tree builds against feature-2.3.6-base, where the exception
// is tagged STRING_EXCEPT_PRIME_TOWER_VARIABLE_LAYER_HEIGHT and hard-fails.)
TEST_CASE("Hollow embedded process preset rejected, project slices clean (train city)", "[integration][preset][embedded]")
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = std::string(ORCA_TEST_FIXTURE_DIR) + "/hollow_embedded_preset_train_city.3mf";
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
