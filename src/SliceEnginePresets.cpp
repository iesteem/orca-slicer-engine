// Part of the SliceEngine class, split from SliceEngine.cpp on 2026-11-13
// (pure code move, no behavior change). Domain: official Snapmaker preset
// system: bundle loading, the printer/filament/process substitution paths
// (incl. the config-overwrite and filament-vector normalization helpers),
// user-content stripping, and project-preset lookups.

#include "SliceEngine.hpp"
#include "FlushVolumes.hpp"
#include "PresetRollback.hpp"
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
// Direct use below (IWYU): boost::filesystem
#include <boost/filesystem.hpp>

using namespace Slic3r;

namespace
{

// Default values used to detect that the official printer preset was NOT
// actually applied (printable_area / printable_height still at library defaults).
constexpr double DEFAULT_PLATE_WIDTH = 200.0;
constexpr double DEFAULT_PLATE_DEPTH = 200.0;
constexpr double DEFAULT_PRINTABLE_HEIGHT = 100.0;

// Tolerance for comparing plate dimensions against the library defaults.
// Direct float == is unreliable; the official U1 bed (~270 mm) is far from
// these defaults, so any small epsilon cleanly separates "applied" from "not".
constexpr double PLATE_DIM_EPSILON = 1e-3;

// Nozzle diameter values that, when within this epsilon of an integer, are
// formatted with 0 decimals (e.g. 1.0 -> "1"); otherwise 1 decimal ("0.4").
constexpr double NOZZLE_FORMAT_EPSILON = 0.05;

// Common implementation for overwriting keys from src onto dst.
// If except is non-null, keys present in the set are skipped.
// Every key (scalar or vector) is copied wholesale — desktop ConfigBase::apply
// semantics. Vectors replace the whole values array, so a source vector
// shorter than dst's truncates dst and an empty source vector clears dst;
// per-filament lengths are reconciled afterwards by
// normalize_filament_vectors_to_slot_count (2026-09-20 C3 fix).
//
// Keys absent from dst are CREATED (option(key, true)) — desktop apply
// semantics installs the whole preset key set. The old skip-absent rule let a
// key the official preset declares but the 3MF omits (enable_filament_ramming,
// purge_in_prime_tower, extruder_clearance_radius, …) fall through to the
// FullPrintConfig defaults backfill in normalize_loaded_config, which runs
// AFTER substitution: the code default masqueraded as the official preset
// value (ramming default true vs U1's declared 0 deepened the wipe tower and
// tripped tower-family TOOLPATH_CONFLICT — W2 rerun, 2026-10-21). The
// filament path (PresetRollback::overwriteExtruderFrom) already created
// absent keys; this aligns the printer/process paths with it. option(key,
// true) returns nullptr for keys unknown to PrintConfig's def — the guard
// below skips those, same as before.
inline void overwrite_all_keys_from_impl(DynamicPrintConfig& dst, const DynamicPrintConfig& src,
                                         const std::set<std::string>* except)
{
    for (auto it = src.cbegin(); it != src.cend(); ++it)
    {
        const auto& key = it->first;
        if (except && except->find(key) != except->end())
            continue;

        ConfigOption* dst_opt = dst.option(key, true);
        if (!dst_opt)
            continue;

        // ConfigOptionSingle::set() throws ConfigurationError when the
        // source type differs. Guard against same-key/different-type
        // mismatches.
        if (dst_opt->type() != it->second->type())
            continue;
        dst_opt->set(it->second.get());
    }
}

// Overwrite every key present in `src` onto `dst` (absent keys are created).
// Scalars and vectors are copied wholesale. No user value is preserved.
inline void overwrite_all_keys_from(DynamicPrintConfig& dst, const DynamicPrintConfig& src)
{
    overwrite_all_keys_from_impl(dst, src, nullptr);
}

// Overwrite every key present in `src` onto `dst`, except keys listed in `except`.
// Same copy semantics as overwrite_all_keys_from.
inline void overwrite_all_keys_from_except(DynamicPrintConfig& dst, const DynamicPrintConfig& src,
                                           const std::set<std::string>& except)
{
    overwrite_all_keys_from_impl(dst, src, &except);
}

// Normalize every per-filament vector in `config` to the project's slot
// count — the engine-side port of Preset::normalize's filament branch
// (Preset.cpp), which desktop runs after every preset load/apply and the
// engine skipped (2026-09-20 C3 fix).
//
// Divergence from desktop, on purpose: the slot count n comes from
// filament_settings_id — the authority the 1.4b substitution loop already
// iterates and the desktop Plater equally uses to size filament presets —
// not from filament_diameter.size() as Preset::normalize does. A truncated
// or foreign-slot-count 3MF is exactly the input this repairs, and its
// filament_diameter is precisely the corrupted length we must not trust.
//
// Semantics (mirrors Preset::normalize as it was designed):
//   1. Exact-resize the ~23 core filament_option_keys with FullPrintConfig
//      defaults padding (grows AND shrinks; skips the flow-variant segmented
//      keys) — see the inline note for why this is engine-side code.
//   2. Grow-only pass over Preset::filament_options(): backfills missing
//      keys from defaults, pads short vectors to n (flow-variant keys to
//      their segment count instead).
//   3. Grows filament_settings_id to n (no-op when it is the authority).
// filament_ids (the AMS material-id array) is deliberately untouched —
// stats readers own it. nozzle_diameter / set_num_extruders is deliberately
// not ported: extruder-count keys belong to the official machine preset.
// Returns the slot count (0 when the config declares none).
size_t normalize_filament_vectors_to_slot_count(DynamicPrintConfig& config)
{
    size_t n = 0;
    if (const auto* fsi = config.option<ConfigOptionStrings>("filament_settings_id", false))
        n = fsi->values.size();
    if (n == 0)
        for (const char* key : {"filament_diameter", "filament_colour"})
            if (const auto* o = config.option(key, false))
                if (const auto* v = dynamic_cast<const ConfigOptionVectorBase*>(o))
                    n = std::max(n, v->size());
    if (n == 0)
        return 0;

    // Exact-resize the core per-filament keys (grow AND shrink). The library's
    // DynamicPrintConfig::set_num_filaments is a complete no-op in this fork —
    // PrintConfigDef's ctor never calls init_filament_option_keys() (zero
    // callers tree-wide), so print_config_def.filament_option_keys() is always
    // empty and its loop iterates nothing. Desktop gets away with it because
    // the GUI layer sizes these keys via PresetBundle::set_num_filaments; the
    // engine has no GUI layer, so the exact-resize lives here, mirroring the
    // key list of PrintConfigDef::init_filament_option_keys (PrintConfig.cpp)
    // with the same skips (flow-variant segmented keys, default profile).
    static const std::vector<std::string> core_filament_keys = {
        "filament_diameter", "min_layer_height", "max_layer_height",
        "retraction_length", "z_hop", "z_hop_types", "retract_lift_above", "retract_lift_below",
        "retract_lift_enforce", "retraction_speed", "deretraction_speed",
        "retract_before_wipe", "retract_restart_extra", "retraction_minimum_travel",
        "wipe", "wipe_distance",
        "retract_when_changing_layer", "retract_length_toolchange", "retract_restart_extra_toolchange",
        "filament_colour", "filament_multi_colors", "filament_colour_mode",
        "retraction_distances_when_cut", "long_retractions_when_cut"};
    const auto& defaults = FullPrintConfig::defaults();
    for (const std::string& key : core_filament_keys)
    {
        // Defense-in-depth only: core_filament_keys is disjoint from
        // filament_flow_variant_options() by construction (the flow-variant
        // segmented keys are deliberately kept out of this list — see the
        // comment above). The predicate stays so a future variant key added
        // to the list is skipped rather than mis-resized. The two literal
        // keys this guard used to check (filament_flow_support /
        // filament_flow_step_size) never appear in core_filament_keys — that
        // skip is only live in the Preset::filament_options loop below.
        if (is_filament_flow_variant_option(key))
            continue;
        auto* opt = config.option(key, false);
        if (opt != nullptr && opt->is_vector())
            static_cast<ConfigOptionVectorBase*>(opt)->resize(n, defaults.option(key));
    }

    // Flow-variant segment count (Preset::normalize logic): step sizes
    // composed per slot when present, else the declaration list at n == 1,
    // else n.
    size_t flow_count = n;
    if (n == 1)
    {
        if (const auto* ffs = config.option<ConfigOptionStrings>("filament_flow_support", false);
            ffs && !ffs->values.empty())
            flow_count = ffs->values.size();
    }
    else if (const auto* st = config.option<ConfigOptionInts>("filament_flow_step_size", false);
             st && st->values.size() == n)
    {
        flow_count = 0;
        for (int s : st->values)
            flow_count += size_t(std::max(1, s));
    }

    for (const std::string& key : Preset::filament_options())
    {
        if (key == "compatible_prints" || key == "compatible_printers" ||
            key == "filament_flow_support")
            continue;
        // Create absent keys through the config's own def (option(key,true) →
        // create_default_option), NOT by cloning the static FullPrintConfig
        // member. The static members are default-constructed and the coEnums
        // ones carry keys_map == nullptr for their whole life (zero
        // assignment sites in PrintConfig.hpp); clone() copies that null
        // through and set_key_value stores it wholesale, bypassing the
        // create_default_option() keys_map fixup. A config built that way
        // slices fine but dies at gcode export, where
        // ConfigOptionEnumsGenericTempl::serialize walks *this->keys_map
        // (87fa2fa third-party 3MF, 2026-10-22: minimal config block →
        // overhang_fan_threshold created null-keyed [50%x4], load and all
        // 749 layers OK, SIGSEGV in append_full_config). The def default
        // values are identical to the static member's, so this only adds the
        // fixup.
        ConfigOption* opt = config.option(key, true);
        if (!opt)
            continue;
        if (opt->is_vector())
        {
            auto* vec = static_cast<ConfigOptionVectorBase*>(opt);
            const size_t want = is_filament_flow_variant_option(key) ? flow_count : n;
            if (vec->size() < want)
                vec->resize(want, defaults.option(key));
        }
    }

    if (auto* fsi = config.option<ConfigOptionStrings>("filament_settings_id", false))
        if (fsi->values.size() < n)
            fsi->values.resize(n, std::string());

    return n;
}

} // namespace

void SliceEngine::load_system_presets()
{
    std::string profiles_path = Slic3r::data_dir();
    if (profiles_path.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "data_dir not set; skipping preset validation";
        return;
    }
    boost::filesystem::path profiles_dir(profiles_path);
    if (!boost::filesystem::exists(profiles_dir) || !boost::filesystem::is_directory(profiles_dir))
    {
        BOOST_LOG_TRIVIAL(warning) << "Profiles directory not found: " << profiles_dir.string()
                                   << "; skipping preset validation";
        return;
    }

    // Collect vendor JSON files
    std::vector<std::string> vendor_names;
    for (const auto& entry : boost::filesystem::directory_iterator(profiles_dir))
    {
        std::string file = entry.path().string();
        if (!Slic3r::is_json_file(file))
            continue;
        std::string name = entry.path().filename().string();
        name.erase(name.size() - 5); // strip .json
        vendor_names.push_back(name);
    }
    if (vendor_names.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "No vendor JSON files in " << profiles_dir.string()
                                   << "; skipping preset validation";
        return;
    }

    try
    {
        m_preset_bundle = std::make_unique<Slic3r::PresetBundle>();

        // In validation mode, load_vendor_configs_from_json reads presets
        // into this PresetBundle. We load all vendors directly (no merge_presets
        // needed — duplicate detection is not critical for cloud validation).
        const auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
        for (size_t i = 0; i < vendor_names.size(); ++i)
        {
            const std::string& vendor = vendor_names[i];
            // First vendor: no base_bundle. Subsequent: pass this bundle for
            // cross-vendor preset inheritance resolution.
            const PresetBundle* base = (i == 0) ? nullptr : m_preset_bundle.get();
            m_preset_bundle->load_vendor_configs_from_json(profiles_dir.string(), vendor, PresetBundle::LoadSystem,
                                                           rule, base);
        }

        m_presets_available = true;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(warning) << "Failed to load system presets: " << e.what() << "; preset validation skipped";
        m_preset_bundle.reset();
        m_presets_available = false;
    }
}

bool SliceEngine::ensure_presets_available()
{
    if (m_presets_available && m_preset_bundle)
        return true;

    record_stage_error(EXIT_PREPROCESS_ERROR, "PRESETS_MISSING",
                       "System presets not available; cannot slice.");
    return false;
}

// Unified official-preset predicate shared by the printer, filament, and
// process substitution paths. See declaration in SliceEngine.hpp.

bool SliceEngine::is_official_preset(const Preset& p)
{
    return p.vendor && p.vendor->name == PresetBundle::SM_BUNDLE;
}

const Preset* SliceEngine::find_official_by_name(const PresetCollection& collection, const std::string& name)
{
    // Exact-name match first. load_vendor_configs_from_json does not rebuild
    // the renamed-from map (update_system_maps is only called from
    // load_configbundle / load_installed_printers on the desktop path), so a
    // legacy name carried by an older 3MF must also be matched by linear scan
    // over each preset's public renamed_from list (official profiles renamed
    // e.g. "0.20 Standard" -> "0.20mm Standard" in Aug 2026).
    auto by_name = [&collection](const std::string& n) -> const Preset* {
        const Preset* p = collection.find_preset(n, false);
        if (p && p->name == n && !p->is_project_embedded && is_official_preset(*p)) return p;
        return nullptr;
    };
    if (const Preset* p = by_name(name)) return p;
    if (name.empty()) return nullptr;
    for (const Preset& p : collection.get_presets())
    {
        if (p.is_project_embedded || !is_official_preset(p)) continue;
        for (const std::string& old : p.renamed_from)
            if (old == name) return &p;
    }
    return nullptr;
}

Preset* SliceEngine::find_project_preset(const std::string& name, Preset::Type type) const
{
    // Project-waypoint lookup shared by the filament (resolve_filament,
    // TYPE_FILAMENT) and process (find_official_process_preset, TYPE_PRINT)
    // official-substitution walks — each used to carry its own copy of this
    // loop (smell C3, 2026-11-06). Returns a mutable Preset* (pointer copies
    // out of a const method are fine); read-only callers assign to const.
    for (auto* pp : m_project_presets)
    {
        if (pp && pp->name == name && pp->type == type)
            return pp;
    }
    return nullptr;
}

// ============================================================================
// Stage 1.4a: Printer preset substitution (official Snapmaker U1)
// ============================================================================

bool SliceEngine::apply_preset_substitution()
{
    // Bundle precondition is enforced by the caller: every entry point (run,
    // run_preset_substitution_only, run_geometry_preprocess_only) aborts via
    // ensure_presets_available() before reaching here, so the bundle is
    // guaranteed present (precedent P-09 — init_print would otherwise
    // dereference a null bundle and crash with SIGSEGV).

    // Strip user-supplied content (G-code, notes, post_process, external
    // file refs) before any apply_*_official_preset stage. The three apply
    // stages then restore official values for the keys each owns. When the
    // PRESETS_MISSING error path fires (in ensure_presets_available),
    // strip_user_content is never reached, so no misleading
    // USER_CONTENT_CLEARED tip is emitted — the user content stays
    // untouched and the engine fails cleanly.
    strip_user_content();

    // Size every per-filament vector to the project's slot count before the
    // substitution stages run: a 3MF authored on a different slot count (or a
    // hand-edited one) can carry truncated vectors, and the 1.4b per-slot
    // loop would silently skip the overflow slots (C3). Idempotent with the
    // re-normalization after the printer overwrite below.
    normalize_filament_vectors_to_slot_count(m_config);

    // Apply the official Snapmaker U1 printer preset — wholesale-replaces
    // printer config (printable_area, machine G-code, nozzle_diameter,
    // etc.) with official values.
    if (!apply_printer_official_preset())
        return false;

    // Filament official compliance check & substitution (always enforced).
    // Runs after printer preset so PresetRollback reads the corrected
    // nozzle_diameter from the official config, not the user's 3MF value.
    if (!apply_filament_official_preset())
        return false;

    // Substitute the process preset with the official Snapmaker U1 preset
    // so that process-level settings (skirt_loops, brim_type, etc.) from a
    // different printer profile in the 3MF don't leak through. User-explicit
    // overrides (different_settings_to_system) are preserved.
    // Non-blocking: substitution failure is a warning, not a fatal error.
    apply_process_official_preset();
    return true;
}

// Normalise flush_volumes_matrix / flush_volumes_vector to match the actual
// filament slot count. 3MF files authored by editing the AMS colour palette
// sometimes ship a flush matrix whose dimension no longer matches the filament
// count (e.g. a 3x3 matrix with 4 filament slots). The desktop GUI repairs this
// in PresetBundle::update_multi_material_filament_presets; the headless engine
// has no GUI, so it must do the same here. Without it, Print::_make_wipe_tower
// (libslic3r) hits its `filament_diameter.size() > sqrt(matrix.size())` guard,
// skips wipe-tower/tool-ordering construction, and the subsequent skirt&brim
// step dereferences an empty ToolOrdering -> SIGSEGV. Ported from
// PresetBundle.cpp:3650-3675 (upstream OrcaSlicer). The pad/shrink + rebuild
// algorithm lives in orca::normalize_flush_volumes (FlushVolumes.hpp,
// header-only pure-vector core) so the pair-count invariant is unit-testable
// from engine-tests, which does not link libslic3r.
static void normalize_flush_volumes_matrix(DynamicPrintConfig& config)
{
    constexpr double kEps = 1e-4; // matches libslic3r EPSILON (libslic3r.h)

    const auto* fd_opt = config.option<ConfigOptionFloats>("filament_diameter");
    if (!fd_opt) return;
    const size_t num_filaments = fd_opt->values.size();
    if (num_filaments == 0) return;

    auto* m_opt = config.option<ConfigOptionFloats>("flush_volumes_matrix");
    if (!m_opt) return; // absent → backfill already supplied the 4x4 default
    const std::vector<double> old_matrix = m_opt->values;
    const size_t old_n = static_cast<size_t>(std::sqrt(static_cast<double>(old_matrix.size())) + kEps);
    if (num_filaments == old_n) return;

    auto* v_opt = config.option<ConfigOptionFloats>("flush_volumes_vector");
    std::vector<double> filaments = v_opt ? v_opt->values : std::vector<double>{};
    m_opt->values = orca::normalize_flush_volumes(filaments, old_matrix, num_filaments);
    if (v_opt) v_opt->values = std::move(filaments);
}

void SliceEngine::normalize_loaded_config()
{
    // (a) Backfill any PrintConfig keys the 3MF / preset chain left undefined.
    // Print::apply() and GCode::_do_export() dereference several options
    // without a null check (e.g. seam_slope_type, thumbnails), so a config
    // that omits them SEGVs deep inside libslic3r. The desktop GUI never sees
    // this because it always operates on a full PrintConfig; the CLI path
    // must add the missing keys from FullPrintConfig defaults itself. Only
    // keys absent from m_config are taken — values the project/preset
    // already set are kept.
    {
        const auto& defaults = FullPrintConfig::defaults();
        t_config_option_keys missing;
        for (const std::string& key : defaults.keys())
            if (!m_config.has(key))
                missing.emplace_back(key);
        if (!missing.empty())
            m_config.apply_only(defaults, missing, true);
    }

    // (b) Repair a flush_volumes_matrix whose dimension doesn't match the
    // filament slot count (common in 3MFs whose AMS palette was edited).
    // See normalize_flush_volumes_matrix above.
    normalize_flush_volumes_matrix(m_config);
}

void SliceEngine::strip_user_content()
{
    // Strip user-supplied content from cloud slices for safety and consistency.
    //
    // Two reporting tiers:
    //   - Silent (categories 1, 2, 4: G-code + post_process): no issue emitted.
    //     The official replacement is already reported by apply_*_official_preset
    //     warnings (PRINTER_SUBSTITUTED / FILAMENT_SUBSTITUTED / PROCESS_SUBSTITUTED),
    //     so a duplicate tip here would be noise.
    //   - USER_CONTENT_CLEARED tip (categories 3, 5: notes + external file refs):
    //     emitted once when any field in these categories is cleared. No official
    //     counterpart exists, so without this tip the user would not know the
    //     content was dropped.
    //
    // Categories (config types differ, so each has its own clearing loop):
    //   (1) Scalar G-code (ConfigOptionString): machine + process level.
    //   (2) Per-extruder G-code (ConfigOptionStrings): filament_start/end_gcode.
    //   (3) User-authored text (notes): printer_notes (scalar),
    //       filament_notes (per-extruder).
    //   (4) Shell-command RCE: post_process (list of commands).
    //   (5) External file references: load_custom_gcodes / load_slicedata /
    //       load_settings / load_assemble_list.
    //
    // After (1) and (2), official values are restored by the three apply_*
    // functions, all of which overwrite unconditionally (filament included,
    // even when the user already picked an official preset). Categories (3)
    // through (5) have no official counterpart to restore; the cleared state
    // is the final state.
    //
    // Note on per-extruder vector keys (ConfigOptionStrings): filament preset
    // JSON files store these as single-element arrays because a filament
    // preset describes ONE material. At runtime, libslic3r expands them to
    // one slot per extruder based on the merged printer config (U1 0.4 has
    // 4 extruders → 4 slots). clear_vector preserves the slot count and only
    // blanks each entry — collapsing the vector would break per-extruder
    // indexing downstream.

    // Clears a scalar ConfigOptionString key. Returns true if the key existed
    // and had non-empty content.
    auto clear_scalar = [this](const char* key) -> bool
    {
        auto* opt = m_config.option<ConfigOptionString>(key, false);
        if (!opt || opt->value.empty())
            return false;
        opt->value.clear();
        return true;
    };
    // Clears all slots of a ConfigOptionStrings key (preserving vector length
    // — per-extruder keys must keep one entry per extruder). Returns true if
    // any slot was non-empty.
    auto clear_vector = [this](const char* key) -> bool
    {
        auto* opt = m_config.option<ConfigOptionStrings>(key, false);
        if (!opt)
            return false;
        bool any = false;
        for (std::string& v : opt->values)
        {
            if (!v.empty())
            {
                v.clear();
                any = true;
            }
        }
        return any;
    };

    // G-code, post_process, and notes (categories 1-4) are all silent —
    // each has an official counterpart in the corresponding preset
    // (machine/process/filament JSON) and is restored by the three
    // apply_*_official_preset warnings (PRINTER_SUBSTITUTED /
    // FILAMENT_SUBSTITUTED / PROCESS_SUBSTITUTED). Notes belong to printer
    // and filament presets; post_process belongs to the process preset. A
    // separate tip here would duplicate the SUBSTITUTED message.

    // (1) Scalar G-code. Canonical PrintConfig names only — Marlin-era bare
    // variants (start_gcode, toolchange_gcode, …) are not registered keys and
    // would be dead entries.
    constexpr const char* scalar_gcode_keys[] = {
        "machine_start_gcode",
        "machine_end_gcode",
        "before_layer_change_gcode",
        "layer_change_gcode",
        "change_filament_gcode",
        "machine_pause_gcode",
        "change_extrusion_role_gcode",
        "template_custom_gcode",
        "printing_by_object_gcode",
        "time_lapse_gcode",
        "print_host",
    };
    for (const char* key : scalar_gcode_keys)
        clear_scalar(key);

    // (2) Per-extruder G-code
    constexpr const char* vector_gcode_keys[] = {
        "filament_start_gcode",
        "filament_end_gcode",
    };
    for (const char* key : vector_gcode_keys)
        clear_vector(key);

    // (3) User-authored text — restored by apply_printer_official_preset
    // (printer_notes) and apply_filament_official_preset (filament_notes).
    clear_scalar("printer_notes");
    clear_vector("filament_notes");

    // (4) post_process: ConfigOptionStrings (list of shell commands) — belongs
    // to the process preset, restored by apply_process_official_preset.
    // Reset to an empty vector — unlike per-extruder keys, the slot count
    // carries no semantic meaning here.
    if (auto* pp = m_config.option<ConfigOptionStrings>("post_process", false))
    {
        if (!pp->values.empty())
            m_config.set_key_value("post_process", new ConfigOptionStrings({}));
    }

    // (5) External file references — the only category with no official
    // counterpart. These keys never appear in any official Snapmaker preset
    // JSON; they exist only as CLI flags (--load-settings, --load-custom-gcodes,
    // …) consumed by Snapmaker_Orca.cpp at the CLI layer. Without an official
    // replacement, cleared content would silently disappear, so the
    // USER_CONTENT_CLEARED tip is the user's only signal.
    bool user_only_content = false;
    user_only_content |= clear_scalar("load_custom_gcodes");
    user_only_content |= clear_scalar("load_assemble_list");
    for (const char* key : {"load_slicedata", "load_settings"})
    {
        auto* opt = m_config.option<ConfigOptionStrings>(key, false);
        if (opt && !opt->values.empty())
        {
            m_config.set_key_value(key, new ConfigOptionStrings({}));
            user_only_content = true;
        }
    }

    if (user_only_content)
    {
        m_stats.issues.push_back(make_tip(
            -1, "USER_CONTENT_CLEARED",
            "External file references in config cleared for cloud safety."));
    }
}

bool SliceEngine::apply_printer_official_preset()
{
    // Replace the user's printer configuration wholesale with the official
    // Snapmaker U1 preset matching the requested nozzle diameter. No user
    // printer value (printable area, machine G-code, kinematics, …) is kept.
    // The official preset config is sourced from the already-loaded
    // PresetBundle, whose system presets carry a fully inherits-expanded
    // config (fdm_U1 -> fdm_toolchanger merged in at load time).
    //
    // Precondition: bundle availability is verified by run() before this
    // function is called, so m_preset_bundle is non-null here.

    // Determine nozzle diameter from the first extruder (default 0.4).
    double nozzle = 0.4;
    const ConfigOptionFloats* nd = m_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nd && !nd->values.empty())
        nozzle = nd->values[0];

    // Look up the official preset in the loaded bundle. find_preset returns
    // nullptr when not found (first_visible_if_not_found = false).
    auto fmt_nozzle = [](double d)
    {
        std::array<char, 8> buf;
        const int precision = (std::abs(d - std::round(d)) < NOZZLE_FORMAT_EPSILON) ? 0 : 1;
        snprintf(buf.data(), buf.size(), "%.*f", precision, d);
        return std::string(buf.data());
    };
    std::string preset_name = "Snapmaker U1 (" + fmt_nozzle(nozzle) + " nozzle)";

    // Priority 1: inherits_group[last] — direct pointer to the official
    // printer preset that the user's printer inherits from. Recorded by
    // Orca desktop at export time, so more reliable than reconstructing
    // from nozzle diameter. Falls back to the nozzle-based name if the
    // field is absent, empty, or does not resolve to an official Snapmaker
    // printer preset.
    //
    // DO NOT "unify" with the process walk's inherits_group[0] below
    // (apply_process_official_preset): the SAME array is read from OPPOSITE
    // ends by design — printer parent = LAST element, process parent =
    // FIRST element (commit 2febb622; verified against desktop export
    // behavior). The index difference is semantics, not an isomorphic pair
    // (smell C3 group b).
    std::string official_printer_name;
    if (auto* ig_opt = m_config.option<ConfigOptionStrings>("inherits_group"))
    {
        if (!ig_opt->values.empty())
        {
            const std::string& parent = ig_opt->values.back();
            if (!parent.empty())
            {
                const Preset* candidate = SliceEngine::find_official_by_name(m_preset_bundle->printers, parent);
                if (candidate)
                {
                    official_printer_name = candidate->name;  // may be the renamed-to name
                }
            }
        }
    }
    if (official_printer_name.empty())
        official_printer_name = preset_name;

    const Preset* official = SliceEngine::find_official_by_name(m_preset_bundle->printers, official_printer_name);
    if (!official)
    {
        record_stage_error(EXIT_PREPROCESS_ERROR, "PRINTER_PRESET_LOAD_ERROR",
                           "Official printer preset not found for nozzle " + fmt_nozzle(nozzle) + " mm (looked for \"" +
                               preset_name + "\").");
        return false;
    }

    // Capture the user's original printer preset name before the wholesale
    // overwrite below replaces printer_settings_id with the official value.
    // Used to surface "X replaced with Y" in the SUBSTITUTED warning so users
    // can trace which of their presets was discarded — matches the filament
    // and process paths, which already list the original name.
    std::string original_printer;
    if (const auto* pid = m_config.option<ConfigOptionString>("printer_settings_id", false))
        original_printer = pid->value;

    // nozzle_volume_type records which physical nozzle (standard / high_flow)
    // is mounted on each extruder. The official presets are keyed by nozzle
    // diameter only and never declare this key, so Preset::normalize
    // backfills "standard" when the bundle loads them — the wholesale
    // overwrite below would silently reset a user's high-flow nozzles to
    // standard in the header echo and the packaged project config
    // (2026-09-18 cloud review, defect 2). Snapshot the user's hardware
    // state and graft it back after the overwrite; the official config
    // still owns the slot count.
    std::vector<int> user_nozzle_volume_type;
    if (const auto* nvt = m_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", false))
        user_nozzle_volume_type = nvt->values;

    // Wholesale overwrite: every key in the official config replaces the
    // user's value (machine G-code keys included).
    overwrite_all_keys_from(m_config, official->config);

    // The official machine preset carries filament-length vectors at the
    // machine's own slot count (e.g. U1's size-4 retraction/wipe/z_hop
    // vectors); the wholesale overwrite above installs them verbatim, so
    // reconcile the per-filament lengths back to the project's slot count
    // before 1.4b runs (C3). Never touches nozzle_volume_type, so the graft
    // below still sees the official slot count it expects.
    normalize_filament_vectors_to_slot_count(m_config);

    // Graft the user's nozzle hardware state back onto the official vector
    // (official sets the slot count; user values fill the leading slots).
    if (!user_nozzle_volume_type.empty())
    {
        if (auto* nvt = m_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", false))
        {
            const size_t count = std::min(nvt->values.size(), user_nozzle_volume_type.size());
            std::copy_n(user_nozzle_volume_type.begin(), count, nvt->values.begin());
        }
    }

    if (!verify_printer_geometry())
        return false;

    // Reporting follows the unified substitution policy:
    //   - Same name (user already picked the official preset): the overwrite
    //     is cloud-side hardening against locally-modified copies. Surface
    //     it as "verified against" so the user knows no swap happened but
    //     the preset was reapplied.
    //   - Different name (real substitution): explicit "from X to Y".
    // Both branches emphasise "for cloud safety" — the cloud pipeline
    // overwrites regardless of the user's choice.
    std::string msg;
    if (!original_printer.empty() && original_printer != preset_name)
        msg = "Printer preset substituted from \"" + original_printer + "\" to \"" + preset_name +
              "\" for cloud safety";
    else
        msg = "Printer preset \"" + preset_name + "\" verified against official preset for cloud safety";
    m_stats.issues.push_back(make_warning(-1, "PRINTER_SUBSTITUTED", msg));
    return true;
}

bool SliceEngine::verify_printer_geometry()
{
    // After overwrite_all_keys_from, confirm the official preset actually took
    // effect by checking that printable_area / printable_height are no longer
    // the library defaults. A silent no-op overwrite (e.g. bundle corruption,
    // missing keys) would otherwise proceed to slicing with a 200x200x100 bed
    // and produce nonsense G-code. Treat as a fatal preprocess error.

    auto fail = [&](const std::string& detail)
    {
        record_stage_error(EXIT_PREPROCESS_ERROR, "PRINTER_PRESET_NOT_APPLIED",
                           "Printer configuration incomplete: " + detail +
                               ". The official U1 printer preset was not applied correctly.");
    };

    auto* pa = m_config.option<ConfigOptionPoints>("printable_area");
    if (!pa || pa->values.size() != 4)
    {
        fail("printable_area missing or wrong format");
        return false;
    }
    auto near = [](double a, double b) { return std::abs(a - b) < PLATE_DIM_EPSILON; };
    bool is_default_area =
        near(pa->values[0].x(), 0.0) && near(pa->values[0].y(), 0.0) &&
        near(pa->values[2].x(), DEFAULT_PLATE_WIDTH) && near(pa->values[2].y(), DEFAULT_PLATE_DEPTH);
    if (is_default_area)
    {
        fail("printable_area is still the default");
        return false;
    }
    auto* ph = m_config.option<ConfigOptionFloat>("printable_height");
    if (!ph || near(ph->value, DEFAULT_PRINTABLE_HEIGHT))
    {
        fail("printable_height is still the default");
        return false;
    }
    return true;
}

// ============================================================================
// Stage 1.4b: Filament preset substitution (official compliance check)
// ============================================================================

bool SliceEngine::apply_filament_official_preset()
{
    if (!m_config.has("filament_settings_id"))
        return true;
    auto* filament_ids = m_config.option<ConfigOptionStrings>("filament_settings_id", true);
    if (!filament_ids || filament_ids->values.empty())
        return true;

    // Deduplicated accumulators for filament warnings. Multiple extruders
    // often share the same filament preset (e.g. 4× Generic PLA on a 4-extruder
    // machine) — emitting one warning per slot produces N identical lines.
    // Instead, group slots by (original, target) pair and emit one merged
    // warning per group with the slot indices listed.
    //
    // Errors (rollback fallback failure) are NOT deduplicated — each failed
    // slot deserves its own error line.
    FilamentGrouping rolled_back;
    FilamentGrouping substituted;

    int num_filaments = static_cast<int>(filament_ids->values.size());
    bool any_error = false;
    for (int i = 0; i < num_filaments; ++i)
    {
        if (!resolve_filament(i, filament_ids, rolled_back, substituted))
            any_error = true;
    }

    emit_filament_warnings(rolled_back, substituted);

    if (any_error)
    {
        mark_stage_failure(EXIT_PREPROCESS_ERROR);
    }
    return !any_error;
}

bool SliceEngine::resolve_filament(int i, Slic3r::ConfigOptionStrings* filament_ids,
                                   FilamentGrouping& rolled_back, FilamentGrouping& substituted)
{
    // Decision tree for a single extruder's filament:
    //   Case 1 — direct system preset match
    //       official → overwrite + record substitution (orig==target)
    //       unofficial vendor → rollback (vendor mismatch)
    //   Case 2 — not in system: walk inheritance chain
    //       find official ancestor → overwrite + record substitution
    //       unofficial vendor in chain → rollback
    //       unknown / circular / no ancestor → rollback
    // Rollback failure → emit error immediately (not deduplicated).
    //
    // Even when the user already selected an official filament preset, we
    // still overwriteExtruderFrom its config — cloud-side hardening drops
    // user-modified copies of the official values, symmetric with the printer
    // and process apply paths.

    auto find_in_system = [this](const std::string& name) -> Preset*
    {
        // Same acceptance rule as find_official_process_preset: genuine system
        // presets only (official Snapmaker vendor, not project-embedded), with
        // renamed_from fallback (see find_official_by_name).
        return const_cast<Preset*>(SliceEngine::find_official_by_name(m_preset_bundle->filaments, name));
    };
    // Project-waypoint lookup shared with the process walk
    // (find_official_process_preset) via find_project_preset (smell C3).
    auto find_in_project = [this](const std::string& name) -> Preset*
    {
        return find_project_preset(name, Preset::TYPE_FILAMENT);
    };
    // All non-official failure branches funnel through here: try PresetRollback
    // to fall back to a base category preset. On success, record for deduplicated
    // reporting; on failure, emit the original error immediately.
    // After a successful rollback, filament_ids->values[i] is already updated
    // to the base category name.
    auto try_rollback = [&](const std::string& name, const char* err_code, const std::string& err_msg) -> bool
    {
        if (PresetRollback::rollback(m_config, m_preset_bundle.get(), i))
        {
            rolled_back[{name, filament_ids->values[i]}].push_back(i);
            return true;
        }
        BOOST_LOG_TRIVIAL(error) << err_msg;
        m_stats.issues.push_back(make_error(-1, err_code, err_msg));
        return false;
    };

    // Capture the user's original filament name BEFORE any overwrite.
    // Two timing hazards fixed by defining `name` here (above Case 0) and
    // copying by value instead of by reference:
    //   (1) Case 0 (inherits_group) is now above the original definition
    //       point, so `name` must exist before the Guard block.
    //   (2) overwriteExtruderFrom mutates filament_ids->values[i] to the
    //       source preset name (PresetRollback.cpp:194-195). A `const
    //       std::string&` reference into that vector would observe the
    //       mutated value, making substituted[{name, ...}] record
    //       (target, target) — collapsing the warning to "verified
    //       against" and hiding the real "substituted from X to Y" swap.
    //       Copy by value to snapshot the original name.
    const std::string name = filament_ids->values[i];

    // Case 0: inherits_group — direct pointer to the official system preset
    // that the user's filament inherits from. This is the most reliable
    // signal because Orca desktop records it at export time after already
    // resolving the inheritance chain. Positional mapping:
    //   inherits_group[i+1] is the parent of filament_settings_id[i].
    //
    // Read via ForwardCompatibilitySubstitutionRule::Enable (same mechanism
    // as different_settings_to_system at L1178). The field has no schema
    // registration; grep will not find it.
    //
    // Five guards ensure format drift does not cause silent mis-resolution.
    // Any guard failure → fall through to Case 1/2/rollback below.
    //   Guard 1: ig_opt exists
    //   Guard 2: parent index in bounds (NOT asserting inherits_size == N+2,
    //            because old desktop versions may write shorter arrays)
    //   Guard 3: parent name non-empty (empty = slot IS official)
    //   Guard 4: parent resolves in system presets (exact match)
    //   Guard 5: parent is Snapmaker-official vendor
    if (auto* ig_opt = m_config.option<ConfigOptionStrings>("inherits_group"))
    {
        const int inherits_size = static_cast<int>(ig_opt->values.size());
        const int parent_idx = i + 1;
        if (parent_idx < inherits_size)
        {
            const std::string& parent_name = ig_opt->values[parent_idx];
            if (!parent_name.empty())
            {
                if (Preset* parent = find_in_system(parent_name))
                {
                    if (is_official_preset(*parent))
                    {
                        PresetRollback::overwriteExtruderFrom(m_config, *parent, i, filament_ids);
                        substituted[{name, parent->name}].push_back(i);
                        return true;
                    }
                    // Non-official vendor → fall through to Case 1/2/rollback
                }
                // parent_name not found in system → fall through
            }
            // parent_name empty → this slot IS official → fall through to Case 1
        }
        // parent_idx out of bounds → inherits_group shorter than expected → fall through
    }
    // inherits_group absent → fall through

    // Case 1: Direct system preset match
    if (Preset* sys = find_in_system(name))
    {
        if (is_official_preset(*sys))
        {
            // Official filament: full overwrite (cloud-side hardening drops
            // user-modified copies). Recorded with orig == target so
            // emit_filament_warnings renders it as "verified against".
            PresetRollback::overwriteExtruderFrom(m_config, *sys, i, filament_ids);
            substituted[{name, sys->name}].push_back(i);
            return true;
        }
        return try_rollback(name, "FILAMENT_UNSUPPORTED_VENDOR",
                            "Filament \"" + name + "\" belongs to unsupported vendor");
    }

    // Case 2: Not a direct system match — walk the inheritance chain
    Preset* current = find_in_project(name);
    if (!current)
    {
        return try_rollback(name, "FILAMENT_UNKNOWN", "Filament \"" + name + "\" is not a recognized preset");
    }

    std::set<std::string> visited;
    while (current)
    {
        std::string inherits_name = current->inherits();
        if (inherits_name.empty())
        {
            return try_rollback(name, "FILAMENT_NO_OFFICIAL_ANCESTOR",
                                "Filament \"" + name + "\" is not derived from any Snapmaker or Generic filament");
        }

        if (!visited.insert(inherits_name).second)
        {
            return try_rollback(name, "FILAMENT_CIRCULAR_INHERITS",
                                "Circular inheritance detected in filament \"" + name + "\"");
        }

        // Try system presets first
        if (Preset* parent = find_in_system(inherits_name))
        {
            if (is_official_preset(*parent))
            {
                PresetRollback::overwriteExtruderFrom(m_config, *parent, i, filament_ids);
                substituted[{name, parent->name}].push_back(i);
                return true;
            }
            std::string vendor_name = parent->vendor ? parent->vendor->name : "unknown";
            return try_rollback(name, "FILAMENT_UNSUPPORTED_VENDOR",
                                "Filament \"" + name + "\" derives from unsupported vendor \"" + vendor_name +
                                    "\" via \"" + inherits_name + "\"");
        }

        // Not in system — try project embedded, then continue walking
        Preset* project_parent = find_in_project(inherits_name);
        if (project_parent)
        {
            current = project_parent;
            continue;
        }
        return try_rollback(name, "FILAMENT_UNKNOWN_ANCESTOR",
                            "Filament \"" + name + "\" inherits from unknown preset \"" + inherits_name + "\"");
    }

    __builtin_unreachable(); // loop always exits via return
}

void SliceEngine::emit_filament_warnings(const FilamentGrouping& rolled_back, const FilamentGrouping& substituted)
{
    // Slot list is appended so users can see which extruders were affected;
    // identical rollbacks/substitutions across multiple slots collapse to
    // one line. Message shape follows the unified substitution policy:
    //   orig == target → "verified against" (cloud-side hardening, no swap)
    //   orig != target → "substituted from X to Y" (real swap)
    auto emit_group = [this](const FilamentGrouping& groups, const char* code)
    {
        for (const auto& kv : groups)
        {
            const std::string& orig = kv.first.first;
            const std::string& target = kv.first.second;
            std::string slots_str;
            for (size_t k = 0; k < kv.second.size(); ++k)
            {
                if (k) slots_str += ", ";
                slots_str += std::to_string(kv.second[k]);
            }
            std::string slot_suffix = " (slot" + std::string(kv.second.size() > 1 ? "s " : " ") + slots_str + ")";
            std::string msg;
            if (orig == target)
                msg = "Filament \"" + orig + "\" verified against official preset for cloud safety" + slot_suffix;
            else
                msg = "Filament substituted from \"" + orig + "\" to \"" + target + "\" for cloud safety" + slot_suffix;
            m_stats.issues.push_back(make_warning(-1, code, msg));
        }
    };
    emit_group(rolled_back, "FILAMENT_ROLLED_BACK");
    emit_group(substituted, "FILAMENT_SUBSTITUTED");
}

// ============================================================================
// Stage 1.4c: Process preset substitution (official, preserves user overrides)
// ============================================================================

// Project-owned keys that also live in Preset::print_options: official
// process preset JSONs do not declare them (verified 2026-10: 111/112 files;
// fdm_process_idex.json deliberately declares wipe_tower_rotation_angle=90),
// so Preset::normalize (Preset.cpp:444-452) backfills the FullPrintConfig
// default into official->config. Protect each key ONLY when the official
// value IS that backfill artifact; a declared non-default value (idex=90)
// keeps the official preset as owner. Mirrors s_project_options ∩
// Preset::print_options() (4 keys, PresetBundle.cpp:201 / Preset.cpp:926).
static const std::set<std::string> kProjectOwnedProcessKeys = {
    "wipe_tower_rotation_angle",       // coFloat, default 0     PrintConfig.cpp:6214
    "dithering_local_z_mode",          // coBool,  default false PrintConfig.cpp:4486
    "dithering_local_z_whole_objects", // coBool,  default false PrintConfig.cpp:4493
    "dithering_local_z_infill",        // coBool,  default false PrintConfig.cpp:4501
};

void SliceEngine::apply_process_official_preset()
{
    // Precondition: bundle availability is verified by run() before this
    // function is called, so m_preset_bundle is non-null here.

    // The official printer preset (already applied) sets default_print_profile
    // to the matching Snapmaker U1 process preset name, e.g.
    // "0.20 Standard @Snapmaker U1 (0.4 nozzle)".
    auto* dpp = m_config.option<ConfigOptionString>("print_settings_id", false);
    if (!dpp || dpp->value.empty())
    {
        BOOST_LOG_TRIVIAL(warning)
            << "default_print_profile not set; cannot determine process preset.";
        return;
    }

    const std::string preset_name = dpp->value;

    // Priority 1: inherits_group[0] — direct pointer to the official process
    // preset that the user's process inherits from. Recorded by Orca desktop
    // at export time. Falls back to preset_name if the field is absent, empty,
    // or does not resolve to an official Snapmaker process preset.
    //
    // DO NOT "unify" with the printer walk's inherits_group.back() above
    // (apply_printer_official_preset): the SAME array is read from OPPOSITE
    // ends by design — process parent = FIRST element, printer parent = LAST
    // element (commit 2febb622). The index difference is semantics, not an
    // isomorphic pair (smell C3 group b).
    std::string process_system_name = preset_name;
    if (auto* ig_opt = m_config.option<ConfigOptionStrings>("inherits_group"))
    {
        if (!ig_opt->values.empty())
        {
            const std::string& parent = ig_opt->values[0];
            if (!parent.empty())
            {
                const Preset* candidate = SliceEngine::find_official_by_name(m_preset_bundle->prints, parent);
                if (candidate)
                {
                    process_system_name = candidate->name;  // may be the renamed-to name
                }
            }
        }
    }

    const Preset* official = find_official_process_preset(process_system_name);

    if (official)
    {
        m_last_process_preset_name = official->name;

        std::set<std::string> user_overrides = parse_process_user_overrides();

        BOOST_LOG_TRIVIAL(info)
            << "Applying official process preset \"" << official->name
            << "\" (" << user_overrides.size() << " user overrides preserved)";

        // post_process was cleared by strip_user_content (cloud safety: it is
        // a list of shell commands). The user value is gone, so honouring the
        // override would only block the official value from landing — leaving
        // a vacuum where neither user nor official content lives. The official
        // value is currently "" (latent bug), but drop the override anyway so
        // the official preset owns the key.
        user_overrides.erase("post_process");

        // Project-owned keys (kProjectOwnedProcessKeys) survive the wholesale
        // overwrite only when the official value is the normalize backfill
        // artifact (== code default). Gate per key: a preset that deliberately
        // declares a non-default value (fdm_process_idex's rotation 90) keeps
        // official ownership. Without this, a 3MF's wipe_tower_rotation_angle
        // (e.g. HueForge's 270°) is reset to 0 and the tower rotates out of
        // the bed (P-30). Mirrors the desktop project_config channel
        // (PresetBundle.cpp:3138), which never lets process presets own
        // project-level state.
        {
            const auto& fp_defaults = FullPrintConfig::defaults();
            std::vector<std::string> preserved;
            for (const std::string& key : kProjectOwnedProcessKeys)
            {
                // Single-arg option() is the const-safe lookup (no creation);
                // the two-arg (key, false) overload is non-const.
                const auto* off_opt = official->config.option(key);
                const auto* def_opt = fp_defaults.option(key);
                if (off_opt && def_opt && off_opt->serialize() == def_opt->serialize())
                    if (user_overrides.insert(key).second)
                        preserved.push_back(key);
            }
            if (!preserved.empty())
            {
                std::string joined;
                for (const std::string& key : preserved)
                {
                    if (!joined.empty()) joined += ", ";
                    joined += key;
                }
                BOOST_LOG_TRIVIAL(info)
                    << "Project-owned keys preserved through process substitution: " << joined;
            }
        }

        // Apply every key from the official process preset, except keys the
        // user explicitly overrode and keys that don't exist in the current
        // config.
        overwrite_all_keys_from_except(m_config, official->config, user_overrides);

        // Reporting follows the unified substitution policy (same as printer
        // and filament): same name → "verified against" (cloud-side hardening
        // against user-modified copies); different name → explicit "from X to Y".
        std::string msg;
        if (official->name == preset_name)
            msg = "Process preset \"" + official->name + "\" verified against official preset for cloud safety";
        else
            msg = "Process preset substituted from \"" + preset_name + "\" to \"" + official->name +
                  "\" for cloud safety";
        m_stats.issues.push_back(make_warning(-1, "PROCESS_SUBSTITUTED", msg));
    }
    else
    {
        m_last_process_preset_name.clear();
        BOOST_LOG_TRIVIAL(warning)
            << "No system process preset found for \"" << preset_name
            << "\" (not in system presets and no system ancestor in"
            << " inheritance chain); process settings not updated.";
        // Mirrors the unified substitution policy: unlike printer/filament,
        // a process preset that cannot be resolved to an official system
        // preset is not fatal here — slicing continues with the project
        // config values. Surface it in issues so callers can see the
        // official process preset was never applied.
        m_stats.issues.push_back(make_warning(-1, "PROCESS_NOT_SUBSTITUTED",
            "Process preset \"" + preset_name + "\" does not resolve to any official system preset; "
            "process settings were kept from the project config"));
    }
}

const Preset* SliceEngine::find_official_process_preset(const std::string& preset_name) const
{
    // Look up a process preset by name: system presets first, then project
    // embedded. Follows the same pattern as resolve_filament.

    // "System" here means a genuine system preset: load_project_embedded_presets
    // merges 3MF-embedded presets into m_preset_bundle->prints, so find_preset
    // can return a user-authored embedded preset that merely shares the name
    // space. Such presets are waypoints on an inheritance chain, not official
    // configs safe to apply wholesale.
    auto find_in_system = [this](const std::string& name) -> const Preset*
    {
        // Official = Snapmaker vendor (is_official_preset), with renamed_from
        // fallback for legacy names carried by older 3MFs (see
        // find_official_by_name).
        return SliceEngine::find_official_by_name(m_preset_bundle->prints, name);
    };

    auto find_in_project = [this](const std::string& name) -> const Preset*
    {
        // Shared with the filament walk (resolve_filament) via
        // find_project_preset (smell C3).
        return find_project_preset(name, Preset::TYPE_PRINT);
    };

    // Case 1: Direct system preset match
    if (const Preset* sys = find_in_system(preset_name))
        return sys;

    // Case 2: Not a direct system match — walk the inheritance chain through
    // project-embedded presets to find a *system* preset ancestor. Embedded
    // presets are only waypoints on the chain, never the return value: a
    // user-authored embedded preset (possibly a hollow shell whose only real
    // content is "inherits") must not be applied wholesale as if it were
    // official — that would overwrite valid project config with empty values.
    const Preset* current = find_in_project(preset_name);
    std::set<std::string> visited;
    while (current)
    {
        std::string inherits_name = current->inherits();
        if (inherits_name.empty()) break;

        if (!visited.insert(inherits_name).second)
        {
            BOOST_LOG_TRIVIAL(warning)
                << "Circular inheritance detected in process preset \""
                << preset_name << "\"";
            return nullptr;
        }

        if (const Preset* parent = find_in_system(inherits_name))
            return parent;

        const Preset* project_parent = find_in_project(inherits_name);
        if (project_parent)
        {
            current = project_parent;
            continue;
        }
        break; // unknown ancestor
    }
    return nullptr;
}

std::set<std::string> SliceEngine::parse_process_user_overrides()
{
    // Parse different_settings_to_system[0] — the ;-separated list of process
    // keys the user explicitly changed from the system defaults. Returns an
    // empty set if the field is absent or empty.
    std::set<std::string> user_overrides;
    auto* diff_opt = m_config.option<ConfigOptionStrings>("different_settings_to_system", false);
    if (!diff_opt || diff_opt->values.empty() || diff_opt->values[0].empty())
        return user_overrides;

    std::istringstream ss(diff_opt->values[0]);
    std::string key;
    while (std::getline(ss, key, ';'))
    {
        size_t start = key.find_first_not_of(" \t");
        size_t end   = key.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos)
            key = key.substr(start, end - start + 1);
        if (!key.empty())
            user_overrides.insert(key);
    }
    return user_overrides;
}

// ============================================================================
// Stage 2: Validate input (plate availability)
// ============================================================================

