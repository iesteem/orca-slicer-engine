#include "SliceEngine.hpp"
#include "GeometryCheck.hpp"
#include "Utils.hpp"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <typeinfo>

#include <boost/log/trivial.hpp>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/CustomGCode.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/FilamentHotBedNozzleRules.hpp"

#include "PresetRollback.hpp"

using namespace Slic3r;

namespace
{

// 1/5, same as GUI's LOGICAL_PART_PLATE_GAP
constexpr double LOGICAL_PART_PLATE_GAP = 0.2;

// Check if a plate result indicates a wipe tower tool change mismatch.
// CGAL/float differences on some platforms cause non-consecutive extruder
// ID handling to fail during G-code export.
bool is_wipe_tower_error(const PlateSliceResult& result)
{
    for (const auto& iss : result.issues)
    {
        if (iss.message.find("append_tcr") != std::string::npos)
            return true;
    }
    return false;
}

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
// Scalars are copied wholesale; vectors are copied element-by-element
// up to the shorter length. Only keys that already exist in dst are touched.
inline void overwrite_all_keys_from_impl(DynamicPrintConfig& dst, const DynamicPrintConfig& src,
                                         const std::set<std::string>* except)
{
    for (auto it = src.cbegin(); it != src.cend(); ++it)
    {
        const auto& key = it->first;
        if (except && except->find(key) != except->end())
            continue;

        ConfigOption* dst_opt = dst.option(key, false);
        if (!dst_opt)
            continue;

        if (dst_opt->is_scalar())
        {
            // ConfigOptionSingle::set() throws ConfigurationError when the
            // source type differs. Guard against same-key/different-type
            // mismatches (symmetric with the vector branch below).
            if (dst_opt->type() != it->second->type())
                continue;
            dst_opt->set(it->second.get());
        }
        else
        {
            auto* dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
            auto* src_vec = dynamic_cast<const ConfigOptionVectorBase*>(it->second.get());
            if (!dst_vec || !src_vec)
                continue;
            for (size_t i = 0; i < dst_vec->size() && i < src_vec->size(); ++i)
                dst_vec->set_at(src_vec, i, i);
        }
    }
}

// Overwrite every key present in `src` onto `dst` (only keys that already
// exist in `dst`). Scalars are copied wholesale; vectors are copied
// element-by-element up to the shorter length. No user value is preserved.
inline void overwrite_all_keys_from(DynamicPrintConfig& dst, const DynamicPrintConfig& src)
{
    overwrite_all_keys_from_impl(dst, src, nullptr);
}

// Overwrite every key present in `src` onto `dst`, except keys listed in `except`.
// Same scalar/vector copy semantics as overwrite_all_keys_from.
inline void overwrite_all_keys_from_except(DynamicPrintConfig& dst, const DynamicPrintConfig& src,
                                           const std::set<std::string>& except)
{
    overwrite_all_keys_from_impl(dst, src, &except);
}

} // namespace

SliceEngine::SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files)
    : m_cfg(cfg), m_temp_files(temp_files)
{
}

SliceEngine::~SliceEngine()
{
    release_PlateData_list(m_plate_data);
}

bool SliceEngine::run()
{
    if (!load_3mf())
    {
        build_statistics();
        return false;
    }

    // Block slicing if printer model is not Snapmaker U1.
    // Moved before preset loading to fail fast — this check reads only
    // m_config (populated during 3MF load) and has no preset dependency.
    if (!validate_printer_model())
    {
        build_statistics();
        return false;
    }

    // Collect config warnings (never blocks the pipeline).
    collect_config_warnings();

    // Load vendor preset JSONs from resources/profiles/. Required for the
    // apply_*_official_preset stages below.
    load_system_presets();

    // Merge project-embedded presets (read from the .3mf input) into the
    // bundle so the apply_*_official_preset stages can resolve inheritance
    // references to them. Never blocks.
    load_project_presets();

    if (!m_cfg.skip_preset_substitution)
    {
        if (!apply_preset_substitution())
        {
            build_statistics();
            return false;
        }
    }

    if (validate_input())
    {
        // --- Setup timeout deadline ---
        m_has_timeout = (m_cfg.timeout_seconds > 0);
        if (m_has_timeout)
        {
            m_timeout_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(m_cfg.timeout_seconds);
        }

        try
        {
            // --- Geometry defect detection (once for entire model, before per-plate loop) ---
            {
                auto geom_issues = run_geometry_checks(m_model);
                for (auto& issue : geom_issues)
                {
                    m_stats.issues.push_back(std::move(issue));
                }
            }

            // Desktop parity: normalize each object's Z onto the bed exactly as opening
            // the project in the desktop app would (ensure_on_bed with allow_negative_z,
            // sinking preserved). Done once at load, before geometry checks and the
            // per-plate loop, so every later stage sees the same coordinates the
            // desktop would.
            ensure_models_on_bed();

            decode_plate_thumbnails();

            m_output_path = generate_output_path(m_cfg.input_file, m_cfg.output_base, m_cfg.plate_id, m_cfg.format,
                                                 m_cfg.single_plate);

            // Collect plates to process (internal plate_index is 0-based)
            std::vector<int> plates_to_process;
            if (m_cfg.single_plate)
            {
                plates_to_process.push_back(m_cfg.plate_id - 1); // CLI 1-based → internal 0-based
            }
            else
            {
                for (const auto& pd : m_plate_data)
                    plates_to_process.push_back(pd->plate_index);
            }

            // Process each plate
            for (int plate_id : plates_to_process)
            {
                process_plate(plate_id);
            }

            // Package output — only if no errors occurred
            bool has_output = !m_plate_results.empty();
            if (has_output && !m_any_error && (m_cfg.format == OutputFormat::GCODE_3MF || !m_cfg.single_plate))
                package_output();

            // For single-plate GCODE mode, the file is written directly to m_output_path
            // during export_gcode. Remove it if errors occurred.
            if (m_any_error && m_cfg.single_plate && m_cfg.format == OutputFormat::GCODE)
            {
                boost::filesystem::remove(m_output_path);
            }
        }
        catch (const std::exception& e)
        {
            BOOST_LOG_TRIVIAL(error) << "Unhandled exception in slicing pipeline: " << e.what();
            m_any_error = true;
            set_error_type(EXIT_SLICING_ERROR);
            m_stats.issues.push_back(make_error(
                -1, "INTERNAL_ERROR",
                "An unexpected internal error occurred during slicing. Please try again or contact support."));
        }
        catch (...)
        {
            BOOST_LOG_TRIVIAL(error) << "Unhandled non-standard exception in slicing pipeline";
            m_any_error = true;
            set_error_type(EXIT_SLICING_ERROR);
            m_stats.issues.push_back(
                make_error(-1, "INTERNAL_FATAL", "Unhandled unknown exception in slicing pipeline"));
        }
    }

    // Always build JSON statistics (even on early failure) so
    // success=false and error_message are reflected in JSON output.
    build_statistics();

    return !m_plate_results.empty();
}

// ============================================================================
// Stage 0: Load 3MF
// ============================================================================

bool SliceEngine::load_3mf()
{
    BOOST_LOG_TRIVIAL(info) << "Loading 3MF file...";

    if (!validate_input_file()) return false;
    if (!read_3mf_model()) return false;

    BOOST_LOG_TRIVIAL(info) << "Loaded " << m_model.objects.size() << " object(s)";
    return true;
}

void SliceEngine::record_load_error(const std::string& code, const std::string& msg)
{
    BOOST_LOG_TRIVIAL(error) << msg;
    m_any_error = true;
    set_error_type(EXIT_LOAD_ERROR);
    m_stats.error_message = msg;
    m_stats.issues.push_back(make_error(-1, code, msg));
}

bool SliceEngine::validate_input_file()
{
    // Extension check (case-insensitive: .3mf, .3MF, .3Mf are all valid)
    std::string extension = boost::filesystem::path(m_cfg.input_file).extension().string();
    boost::to_lower(extension);
    if (extension != ".3mf")
    {
        record_load_error(
            "FORMAT_REJECTED",
            "Only .3mf files are supported. Please export your project from Snapmaker Orca Slicer.");
        return false;
    }

    // File size check (configurable via --max-size, default 200MB, 0 = no limit)
    if (m_cfg.max_size_mb <= 0) return true;

    boost::uintmax_t max_file_size = static_cast<boost::uintmax_t>(m_cfg.max_size_mb) * 1024ULL * 1024ULL;
    boost::system::error_code err_code;
    boost::uintmax_t file_size = boost::filesystem::file_size(m_cfg.input_file, err_code);
    if (!err_code && file_size > max_file_size)
    {
        record_load_error("FILE_SIZE_EXCEEDED",
                          "File size exceeds the limit (" + std::to_string(m_cfg.max_size_mb) +
                              " MB). Please simplify the model or reduce face count and try again.");
        return false;
    }
    return true;
}

bool SliceEngine::read_3mf_model()
{
    // Reset substitution state from any prior run
    m_config_substitutions = ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule::Enable);
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                            LoadStrategy::LoadAuxiliary;

    try
    {
        m_model =
            Model::read_from_file(m_cfg.input_file, &m_config, &m_config_substitutions, strategy, &m_plate_data,
                                  &m_project_presets, &m_is_bbl_3mf, &m_file_version, nullptr, nullptr, nullptr, 0);
    }
    catch (const std::exception& e)
    {
        // Detect gcode.3mf output files (no geometry, only pre-sliced G-code).
        // Model::read_from_file() throws "The supplied file couldn't be read
        // because it's empty" when the 3MF has valid XML metadata but zero
        // model objects — typical of a .gcode.3mf slicing result being
        // mistakenly re-submitted as input.
        // FIXME(libslic3r-upstream): substring match on exception message is
        // brittle — a libslic3r error-text change will silently regress the
        // gcode.3mf detection. Consider an explicit signal from read_from_file.
        const std::string what = e.what();
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << what;
        if (what.find("empty") != std::string::npos)
        {
            record_load_error(
                "LOAD_3MF_ERROR",
                "This 3MF file contains no 3D model objects. "
                "It appears to be a gcode.3mf slicing output file, not a project file. "
                "Please upload the original .3mf project file instead.");
        }
        else
        {
            record_load_error("LOAD_3MF_ERROR",
                              "Failed to load 3MF file. The file may be corrupted or in an unsupported format.");
        }

        // Model::read_from_file() may have partially populated output parameters
        // (plate data, project presets) before throwing.  Release them to prevent
        // downstream code from accessing invalid state.
        release_PlateData_list(m_plate_data);
        m_project_presets.clear();
        return false;
    }

    if (m_model.objects.empty())
    {
        record_load_error("MODEL_EMPTY", "3MF file contains no sliceable model objects");
        return false;
    }
    return true;
}

// ============================================================================
// Stage 1.1: Printer model validation (fail-fast, before preset loading)
// ============================================================================

bool SliceEngine::validate_printer_model()
{
    auto fail_model = [&](const std::string& code, const std::string& msg)
    {
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, code, msg));
        return false;
    };

    if (!m_config.has("printer_model"))
    {
        return fail_model("PRINTER_MODEL_MISSING", "Printer model is missing. Only the Snapmaker U1 is supported.");
    }

    std::string printer_model = m_config.opt_string("printer_model");
    if (printer_model != DEFAULT_PRINTER_MODEL)
    {
        return fail_model("PRINTER_MODEL_UNSUPPORTED",
                          "Unsupported printer model: \"" + printer_model + "\". Only the Snapmaker U1 is supported.");
    }

    return true;
}

// ============================================================================
// Stage 1.2: Config warning collection
// ============================================================================

void SliceEngine::collect_config_warnings()
{
    // Validate config values (layer_height, nozzle_diameter, etc.).
    // Use under_cli=false to match desktop GUI behavior — invalid config
    // values produce warnings but do NOT block slicing.
    std::map<std::string, std::string> invalid = m_config.validate(false);
    for (const auto& [key, msg] : invalid)
        m_stats.issues.push_back(make_warning(-1, "CONFIG_INVALID_" + key, msg));

    // Check config substitutions (unknown keys, forward-compat changes)
    if (!m_config_substitutions.empty())
    {
        for (const auto& substitution : m_config_substitutions.substitutions)
        {
            const char* key = substitution.opt_def ? substitution.opt_def->opt_key.c_str() : "?";
            m_stats.issues.push_back(make_warning(-1, "CONFIG_SUBSTITUTION",
                                                  std::string("Config key '") + key +
                                                      "' value was substituted (old: " + substitution.old_value + ")"));
        }

        for (const auto& key : m_config_substitutions.unrecogized_keys)
        {
            m_stats.issues.push_back(make_warning(-1, "CONFIG_UNRECOGNIZED",
                                                  std::string("Unrecognized config key '") + key +
                                                      "' — may be from a newer slicer version"));
        }
    }
}

// ============================================================================
// Stage 1.3: Load presets (system from vendor JSON, project-embedded from .3mf)
// ============================================================================

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

// Project-embedded presets: loaded from the .3mf input file during load_3mf
// (m_project_presets), merged into m_preset_bundle here. Logically part of
// Stage 1.3 — paired with load_system_presets so the apply_*_official_preset
// stages below see a unified bundle of system + project presets.

void SliceEngine::load_project_presets()
{
    // Precondition: m_preset_bundle is populated by load_system_presets().
    // When system presets are unavailable, there is nowhere to merge project
    // presets into, so we skip silently — apply_*_official_preset also
    // early-exits when m_presets_available is false.
    if (!m_presets_available || !m_preset_bundle) return;
    if (m_project_presets.empty()) return;

    PresetBundle& preset_bundle = *m_preset_bundle;
    try
    {
        PresetsConfigSubstitutions preset_subs = preset_bundle.load_project_embedded_presets(
            m_project_presets, ForwardCompatibilitySubstitutionRule::Enable);

        for (const auto& preset_sub : preset_subs)
        {
            for (const auto& substitution : preset_sub.substitutions)
            {
                const char* key = substitution.opt_def ? substitution.opt_def->opt_key.c_str() : "?";
                m_stats.issues.push_back(make_warning(-1, "PRESET_SUBSTITUTION",
                                                      std::string("Embedded preset '") + preset_sub.preset_name +
                                                          "' key '" + key + "' was substituted"));
            }
        }
    }
    catch (const std::exception& e)
    {
        // Graceful degradation: a malformed embedded preset must not abort
        // the pipeline. The apply_*_official_preset stages below tolerate
        // missing references via inheritance-chain fallbacks.
        BOOST_LOG_TRIVIAL(warning) << "Failed to load project embedded presets: " << e.what();
    }
}

// ============================================================================
// Stage 1.4a: Printer preset substitution (official Snapmaker U1)
// ============================================================================

bool SliceEngine::apply_preset_substitution()
{
    // Bundle precondition for all three apply_*_official_preset stages.
    // Without system presets we cannot look up official U1 / filament /
    // process configurations, so the whole substitution block is aborted.
    if (!m_presets_available || !m_preset_bundle)
    {
        std::string msg = "System presets not available; cannot apply official presets.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRESETS_MISSING", msg));
        return false;
    }

    // Strip user-supplied content (G-code, notes, post_process, external
    // file refs) before any apply_*_official_preset stage. The three apply
    // stages then restore official values for the keys each owns.
    // Runs after the bundle check so the PRESETS_MISSING error path does
    // not also emit a misleading USER_CONTENT_CLEARED tip — if we cannot
    // apply official presets, the user content stays untouched and the
    // engine fails cleanly.
    strip_user_content();

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
    std::string official_printer_name;
    if (auto* ig_opt = m_config.option<ConfigOptionStrings>("inherits_group"))
    {
        if (!ig_opt->values.empty())
        {
            const std::string& parent = ig_opt->values.back();
            if (!parent.empty())
            {
                const Preset* candidate = m_preset_bundle->printers.find_preset(parent, false);
                if (candidate && candidate->name == parent &&
                    candidate->vendor && candidate->vendor->name == PresetBundle::SM_BUNDLE)
                {
                    official_printer_name = parent;
                }
            }
        }
    }
    if (official_printer_name.empty())
        official_printer_name = preset_name;

    const Preset* official = m_preset_bundle->printers.find_preset(official_printer_name, false);
    if (!official)
    {
        std::string msg = "Official printer preset not found for nozzle " + fmt_nozzle(nozzle) + " mm (looked for \"" +
                          preset_name + "\").";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRINTER_PRESET_LOAD_ERROR", msg));
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

    // Wholesale overwrite: every key in the official config replaces the
    // user's value (machine G-code keys included).
    overwrite_all_keys_from(m_config, official->config);

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
        std::string msg = "Printer configuration incomplete: " + detail +
                          ". The official U1 printer preset was not applied correctly.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRINTER_PRESET_NOT_APPLIED", msg));
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
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
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

    auto is_official_preset = [](const Preset& p) -> bool
    {
        return p.vendor && p.vendor->name == PresetBundle::SM_BUNDLE;
    };
    auto find_in_system = [this](const std::string& name) -> Preset*
    {
        auto* p = m_preset_bundle->filaments.find_preset(name, false);
        if (p && p->name == name)
            return p;
        return nullptr;
    };
    auto find_in_project = [this](const std::string& name) -> Preset*
    {
        for (auto* pp : m_project_presets)
        {
            if (pp && pp->name == name && pp->type == Preset::TYPE_FILAMENT)
                return pp;
        }
        return nullptr;
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
        apply_auto_brim_fallback();
        return;
    }

    const std::string preset_name = dpp->value;

    // Priority 1: inherits_group[0] — direct pointer to the official process
    // preset that the user's process inherits from. Recorded by Orca desktop
    // at export time. Falls back to preset_name if the field is absent, empty,
    // or does not resolve to an official Snapmaker process preset.
    std::string process_system_name = preset_name;
    if (auto* ig_opt = m_config.option<ConfigOptionStrings>("inherits_group"))
    {
        if (!ig_opt->values.empty())
        {
            const std::string& parent = ig_opt->values[0];
            if (!parent.empty())
            {
                const Preset* candidate = m_preset_bundle->prints.find_preset(parent, false);
                if (candidate && candidate->name == parent &&
                    candidate->vendor && candidate->vendor->name == PresetBundle::SM_BUNDLE)
                {
                    process_system_name = parent;
                }
            }
        }
    }

    const Preset* official = find_official_process_preset(process_system_name);

    if (official)
    {
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
        BOOST_LOG_TRIVIAL(warning)
            << "No system process preset found for \"" << preset_name
            << "\" (not in system presets and no system ancestor in"
            << " inheritance chain); process settings not updated.";
    }

    apply_auto_brim_fallback();
}

const Preset* SliceEngine::find_official_process_preset(const std::string& preset_name) const
{
    // Look up a process preset by name: system presets first, then project
    // embedded. Follows the same pattern as resolve_filament.

    auto find_in_system = [this](const std::string& name) -> const Preset*
    {
        const Preset* p = m_preset_bundle->prints.find_preset(name, false);
        if (p && p->name == name) return p;
        return nullptr;
    };

    auto find_in_project = [this](const std::string& name) -> const Preset*
    {
        for (auto* pp : m_project_presets)
        {
            if (pp && pp->name == name && pp->type == Preset::TYPE_PRINT)
                return pp;
        }
        return nullptr;
    };

    // Case 1: Direct system preset match
    if (const Preset* sys = find_in_system(preset_name))
        return sys;

    // Case 2: Not a direct system match — walk the inheritance chain
    // to find a system preset ancestor.
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

void SliceEngine::apply_auto_brim_fallback()
{
    // When brim_type is auto_brim, set brim_width to 0 so that the
    // fallback path (when the algorithm decides no brim is needed)
    // doesn't generate unwanted brim. The algorithm still sets its
    // own computed width when it determines brim IS needed.
    auto* bt = m_config.option<ConfigOptionEnum<BrimType>>("brim_type", false);
    if (bt && bt->value == btAutoBrim)
    {
        m_config.set_key_value("brim_width", new ConfigOptionFloat(0));
        BOOST_LOG_TRIVIAL(info)
            << "brim_type=auto_brim: brim_width set to 0 to match desktop behaviour";
    }
}

// ============================================================================
// Stage 2: Validate input (plate availability)
// ============================================================================

bool SliceEngine::validate_input()
{
    // Check plate availability
    if (m_plate_data.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "No plate data in 3MF, treating as single plate";
        PlateData* pd = new PlateData();
        pd->plate_index = 0;
        m_plate_data.push_back(pd);
    }

    // Validate requested plate exists
    if (m_cfg.single_plate)
    {
        // Internal plate_index is 0-based (from 3MF import), CLI plate_id is 1-based
        bool plate_found = false;
        for (const auto& pd : m_plate_data)
        {
            if (pd->plate_index + 1 == m_cfg.plate_id)
            {
                plate_found = true;
                break;
            }
        }
        if (!plate_found)
        {
            BOOST_LOG_TRIVIAL(error) << "Plate " << m_cfg.plate_id << " not found in 3MF file";
            {
                std::ostringstream oss;
                oss << "Available plates: ";
                for (size_t i = 0; i < m_plate_data.size(); ++i)
                {
                    if (i > 0)
                        oss << ", ";
                    oss << m_plate_data[i]->plate_index + 1;
                }
                BOOST_LOG_TRIVIAL(warning) << oss.str();
            }
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
            m_stats.error_message = "Requested plate " + std::to_string(m_cfg.plate_id) + " not found in 3MF file";
            m_stats.issues.push_back(make_error(-1, "PLATE_NOT_FOUND", m_stats.error_message));
            return false;
        }
    }

    return true;
}

// ============================================================================
// Stage 3: Global preprocessing (ensure on bed)
// ============================================================================

void SliceEngine::ensure_models_on_bed()
{
    // Seat every object flat on the bed before slicing.
    //
    // The desktop app, when opening a 3MF project, calls
    // ModelObject::ensure_on_bed(allow_negative_z=true) (Plater.cpp), which
    // PRESERVES intentional sinking — a model stored below the bed stays sunk
    // and is sliced clipped at z=0. That is correct for an interactive editor
    // (sinking is a deliberate tool, e.g. flattening a base against the bed),
    // but wrong for cloud slicing: the user uploads a model expecting the whole
    // thing printed and has no way to reposition it. A stored Z that sinks the
    // model would silently drop the bottom of the G-code.
    //
    // So we pass allow_negative_z=false (the same path the Snapmaker CLI takes
    // when its `ensure_on_bed` option is enabled): each object's lowest point is
    // snapped to z=0. Partly-sunk, fully-sunk and floating objects are all
    // corrected. We deliberately diverge from the desktop *default* here; this
    // matches the desktop's force-on-bed behaviour.
    for (ModelObject* obj : m_model.objects)
    {
        if (obj->instances.empty())
            continue;
        // Force fresh bounding-box computation. The cache may be stale from
        // 3MF import before instance transforms were finalized.
        obj->invalidate_bounding_box();

        // Print::apply zeroes the Z component of the instance transform
        // matrix (PrintApply.cpp:155 — "Z offset is discarded to ensure
        // first layer starts at Z=0").  Any Z in the instance offset is
        // silently dropped, which clips objects that rely on it.
        //
        // Fix: bake the instance Z offset into mesh vertices, zero the
        // instance Z offset, then apply ensure_on_bed to the corrected
        // geometry.  The Z survives Print::apply because it lives in
        // mesh-vertex space.
        //
        // translate() moves mesh vertices in LOCAL space, but the instance
        // rotation may flip or reorient the local Z axis.  We must convert
        // the world-space Z shift to local coordinates via the inverse of
        // the instance rotation (matrix without offset).

        // --- Step 1: bake existing instance Z offset into mesh vertices ---
        double inst_z = 0.0;
        for (ModelInstance* inst : obj->instances)
        {
            inst_z = inst->get_offset().z();
            if (std::abs(inst_z) > 1e-4)
                break;
        }

        if (std::abs(inst_z) > 1e-4)
        {
            // Convert world-space (0,0,inst_z) to local space using the
            // inverse of the instance rotation (no offset).
            auto rot_no_off = obj->instances.front()->get_transformation().get_matrix_no_offset();
            Vec3d world_shift(0, 0, inst_z);
            Vec3d local_shift = rot_no_off.inverse() * world_shift;
            obj->translate(local_shift.x(), local_shift.y(), local_shift.z());
            // Zero all instance Z offsets.
            for (ModelInstance* inst : obj->instances)
            {
                Vec3d off = inst->get_offset();
                inst->set_offset(Vec3d(off.x(), off.y(), 0.0));
            }
        }

        // --- Step 2: correct any remaining sinking ---
        {
            obj->invalidate_bounding_box();
            double before = obj->min_z();
            obj->ensure_on_bed(false);
            double after = obj->min_z();
            double z_shift = after - before;

            if (std::abs(z_shift) > 1e-4)
            {
                // Convert world-space z_shift to local (same as step 1).
                auto rot_no_off = obj->instances.front()->get_transformation().get_matrix_no_offset();
                Vec3d world_shift(0, 0, z_shift);
                Vec3d local_shift = rot_no_off.inverse() * world_shift;
                obj->translate(local_shift.x(), local_shift.y(), local_shift.z());
                // Undo the instance-offset change ensure_on_bed just made.
                for (ModelInstance* inst : obj->instances)
                {
                    Vec3d off = inst->get_offset();
                    inst->set_offset(Vec3d(off.x(), off.y(), off.z() - z_shift));
                }
            }

            obj->invalidate_bounding_box();
            if (std::abs(z_shift) > 1e-6 || std::abs(before) > 1e-6)
            {
                BOOST_LOG_TRIVIAL(info) << "ensure_on_bed: object \"" << obj->name << "\" inst_z=" << inst_z
                                        << " z_shift=" << z_shift;
            }
        }
    }
}

// ============================================================================
// Stage 3: Global preprocessing (decode plate thumbnails)
// ============================================================================

void SliceEngine::decode_plate_thumbnails()
{
    // All allocations (vector<unsigned char>, PNG decode buffers) live inside
    // this try block. std::bad_alloc from a malformed/huge PNG is routed to
    // an early function exit rather than propagating -- the engine's calling
    // convention treats thumbnail decode as best-effort: a missing thumbnail
    // is acceptable, an uncaught exception is not (Snapmaker no-throw policy).
    BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: enter, plate_count=" << m_plate_data.size();
    try
    {
        for (auto& pd : m_plate_data)
            decode_one_plate_thumbnail(*pd);
    }
    catch (const std::exception& e)
    {
        // Most likely std::bad_alloc from a PNG buffer allocation. Log and
        // return normally -- partial progress (already-decoded plates) is
        // retained; remaining plates simply get no thumbnail.
        BOOST_LOG_TRIVIAL(error) << "decode_plate_thumbnails: aborted after exception: " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "decode_plate_thumbnails: aborted after unknown exception";
    }
}

void SliceEngine::decode_one_plate_thumbnail(PlateData& pd)
{
    // PNG file signature (PNG spec section 5.2, first 8 bytes are fixed).
    static constexpr unsigned char PNG_SIGNATURE[] =
        {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr std::streamsize PNG_SIGNATURE_LEN = 8;

    // Max bytes accepted for a single plate thumbnail PNG.
    // Derivation: U1 display renders 300x300; measured PNG ~30 KB. 4 MB gives
    // ~130x headroom, covers future 600x600-class screens, still small enough
    // to prevent OOM from a malicious oversized file.
    static constexpr size_t MAX_PNG_SIZE = 4u * 1024u * 1024u;

    // Guard 1: path non-empty and ends in ".png".
    const std::string& raw_path = pd.thumbnail_file;
    if (raw_path.size() < 4 ||
        raw_path.compare(raw_path.size() - 4, 4, ".png") != 0) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", path not .png";
        return;
    }

    // Guard 2: canonicalise and verify path under system temp root.
    // canonical() resolves symlinks and normalises ".." components,
    // preventing path-traversal where the .3mf supplies a thumbnail_file
    // like "backup/../../etc/passwd.png". The system-temp-root check is
    // done against a known temp parent (e.g. "/tmp/") rather than
    // temp_directory_path() because TMPDIR may point to a custom location
    // that differs from libslic3r's own temp dir (backup_path).
    boost::system::error_code ec;
    const boost::filesystem::path resolved = boost::filesystem::canonical(raw_path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", canonical failed (" << raw_path << ")";
        return;
    }

    // Verify the canonical path is under the system-wide temp root.
    const std::string resolved_str = resolved.string();
    bool under_temp = false;
    for (const char* prefix : {"/tmp/", "/var/tmp/", "/private/tmp/"})
        if (resolved_str.find(prefix) == 0) { under_temp = true; break; }
    if (!under_temp) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", path outside temp dir (" << resolved_str << ")";
        return;
    }

    // Guard 3: file size in [1, MAX_PNG_SIZE].
    const boost::uintmax_t file_sz = boost::filesystem::file_size(resolved, ec);
    if (ec || file_sz == 0 || file_sz > MAX_PNG_SIZE) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", bad size " << file_sz;
        return;
    }
    const size_t sz = static_cast<size_t>(file_sz);

    // Guard 4: open and read full file. gcount() validates bytes read
    // (failbit on EOF makes "!ifs" unreliable).
    std::ifstream ifs(resolved.string(), std::ios::binary);
    if (!ifs.is_open()) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", cannot open";
        return;
    }

    std::vector<unsigned char> file_bytes(sz);
    // reinterpret_cast is required: istream::read takes char*, PNG bytes
    // are unsigned char. This is an ifstream API constraint, not type
    // punning; char may alias any type, safe.
    ifs.read(reinterpret_cast<char*>(file_bytes.data()), static_cast<std::streamsize>(sz));
    if (ifs.gcount() != static_cast<std::streamsize>(sz)) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", short read";
        return;
    }
    // ifs closes itself on destruction (RAII).

    // Guard 5: PNG magic bytes.
    bool sig_ok = true;
    for (size_t i = 0; i < static_cast<size_t>(PNG_SIGNATURE_LEN); ++i)
    {
        if (file_bytes[i] != PNG_SIGNATURE[i])
        {
            sig_ok = false;
            break;
        }
    }
    if (!sig_ok) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", bad PNG magic";
        return;
    }

    // Guard 6: PNG decode.
    Slic3r::png::ReadBuf buf{file_bytes.data(), file_bytes.size()};
    Slic3r::png::ImageColorscale img;
    if (!Slic3r::png::decode_colored_png(buf, img)) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", PNG decode failed";
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: success plate " << pd.plate_index
                            << " (" << img.cols << "x" << img.rows << ")";

    // Populate plate_thumbnail only after successful decode (no half-state).
    pd.plate_thumbnail.set(static_cast<unsigned int>(img.cols), static_cast<unsigned int>(img.rows));

    const size_t src_bpp = static_cast<size_t>(img.bytes_per_pixel);
    for (size_t y = 0; y < img.rows; ++y)
    {
        for (size_t x = 0; x < img.cols; ++x)
        {
            const size_t src_idx = (y * img.cols + x) * src_bpp;
            const size_t dst_idx = (y * img.cols + x) * 4;
            pd.plate_thumbnail.pixels[dst_idx + 0] = img.buf[src_idx + 0];
            pd.plate_thumbnail.pixels[dst_idx + 1] = img.buf[src_idx + 1];
            pd.plate_thumbnail.pixels[dst_idx + 2] = img.buf[src_idx + 2];
            pd.plate_thumbnail.pixels[dst_idx + 3] = (src_bpp >= 4) ? img.buf[src_idx + 3] : 255;
        }
    }
}

// ============================================================================
// Stage 4: Process a single plate
// ============================================================================

void SliceEngine::process_plate(int plate_id)
{
    // Guaranteed by run(): apply_printer_official_preset() must succeed first
    // (which in turn requires bundle availability). Not assert-guarded —
    // structural contract, no-op in Release.

    // --- Filter instances for this plate ---
    std::set<int> identify_ids;
    if (!filter_instances(plate_id, identify_ids))
        return;

    // Calculate plate dimensions and origin (done before build-volume check
    // so the check can translate instances into plate-local coordinates).
    // printable_area is guaranteed valid by apply_printer_official_preset().
    const auto* pa = m_config.option<ConfigOptionPoints>("printable_area");
    BoundingBoxf bbox;
    for (const Vec2d& pt : pa->values)
        bbox.merge(pt);
    Vec3d origin = setup_print_origin(plate_id, bbox.size().x(), bbox.size().y());

    // --- Build volume check (uses plate-local coordinates) ---
    if (!run_build_volume_check(plate_id, identify_ids, origin))
        return;

    // --- Timeout check (before heavy slicing work) ---
    if (!check_timeout(plate_id))
        return;

    // --- Create Print ---
    Print print;
    init_print(print);

    // --- Apply model ---
    if (!apply_model(plate_id, print, origin))
        return;

    // --- Assign arrange_order ---
    assign_arrange_order();

    // --- Set global extruder params & speed table ---
    setup_extruder_params(print);

    // --- Validation ---
    if (!run_validation(plate_id, print))
        return;

    // --- Slicing ---
    if (!run_slicing(plate_id, print))
        return;

    BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << (plate_id + 1);

    // --- Export G-code ---
    PlateSliceResult slice_result;
    if (!export_gcode(plate_id, print, slice_result))
        return;

    // --- Empty G-code layers check ---
    if (!check_empty_gcode_layers(plate_id, slice_result))
        return;

    // --- Post-processing ---
    do_postprocessing(plate_id, slice_result);

    // --- Finalise ---
    finalise_plate_result(plate_id, slice_result);
}

// ============================================================================
// Per-plate sub-stages (in call order)
// ============================================================================

bool SliceEngine::filter_instances(int plate_id, std::set<int>& identify_ids)
{
    for (const auto& pd : m_plate_data)
    {
        if (pd->plate_index == plate_id)
        {
            for (const auto& [object_id, inst_info] : pd->obj_inst_map)
            {
                identify_ids.insert(inst_info.second);
            }
            break;
        }
    }

    int count = 0;
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            bool on_plate = (identify_ids.find(static_cast<int>(inst->loaded_id)) != identify_ids.end());
            inst->printable = on_plate;
            inst->print_volume_state = on_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
            if (on_plate)
                ++count;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Filtered model: " << count << " instances on plate " << (plate_id + 1);

    if (count == 0)
    {
        BOOST_LOG_TRIVIAL(warning) << "Skipping empty plate " << (plate_id + 1);
        return false;
    }
    return true;
}

Slic3r::Vec3d SliceEngine::setup_print_origin(int plate_id, double plate_width, double plate_depth)
{
    // Compute plate origin using the same grid layout formula as the desktop GUI
    // (PartPlate::update_plate_layout_arrange). Each plate occupies a cell in a
    // row-major grid with LOGICAL_PART_PLATE_GAP spacing between plates.
    int total_plates = static_cast<int>(m_plate_data.size());
    int cols = compute_column_count(total_plates);
    int row = plate_id / cols;
    int col = plate_id % cols;

    double origin_x = col * (plate_width * (1.0 + LOGICAL_PART_PLATE_GAP));
    double origin_y = -row * (plate_depth * (1.0 + LOGICAL_PART_PLATE_GAP));

    Vec3d origin(origin_x, origin_y, 0.0);
    return origin;
}

bool SliceEngine::run_build_volume_check(int plate_id, const std::set<int>& identify_ids, const Vec3d& origin)
{
    if (!(m_config.has("printable_area") && m_config.has("printable_height")))
        return true;

    auto printable_area_opt = m_config.option<ConfigOptionPoints>("printable_area");
    double printable_height = m_config.opt_float("printable_height");
    if (!printable_area_opt || printable_area_opt->values.empty() || printable_height <= 0)
        return true;

    // Translate build-volume check into this plate's local coordinate system.
    // Instances on different plates have grid-layout offsets in global space;
    // subtracting the plate origin gives their local position within the plate.
    BuildVolume build_volume(printable_area_opt->values, printable_height);

    // Temporarily shift on-plate instances into plate-local coordinates for the check,
    // then restore them afterwards.
    std::vector<std::pair<ModelInstance*, Vec3d>> shifted;
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            if (!inst->printable)
                continue;
            int lid = static_cast<int>(inst->loaded_id);
            if (identify_ids.find(lid) != identify_ids.end())
            {
                Vec3d global_offset = inst->get_offset();
                Vec3d local_offset = global_offset - origin;
                shifted.emplace_back(inst, global_offset);
                inst->set_offset(local_offset);
            }
        }
    }

    m_model.update_print_volume_state(build_volume);

    bool has_partly_outside = false;
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            if (!inst->printable)
                continue;
            int lid = static_cast<int>(inst->loaded_id);
            if (identify_ids.find(lid) == identify_ids.end())
                continue;

            if (inst->print_volume_state == ModelInstancePVS_Partly_Outside)
            {
                log_plate_message("[Pre-processing]", "ERROR", plate_id,
                                  "Object \"" + obj->name +
                                      "\" is placed on the boundary of or exceeds the build volume.");
                has_partly_outside = true;
                m_stats.issues.push_back(
                    make_error(plate_id, "BUILD_VOLUME_PARTLY_OUTSIDE",
                               "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume",
                               obj->name));
            }
            else if (inst->print_volume_state == ModelInstancePVS_Fully_Outside)
            {
                m_stats.issues.push_back(make_warning(
                    plate_id, "BUILD_VOLUME_FULLY_OUTSIDE",
                    "Object \"" + obj->name + "\" is completely outside the build volume and will not be printed",
                    obj->name));
            }
        }
    }

    // Snapmaker: SpiralLiftNearBoundary warning (matches desktop 3DScene.cpp:1105-1122)
    check_spiral_lift_near_boundary(plate_id, build_volume, identify_ids);

    // Restore global offsets for on-plate instances and update printable state
    for (auto& [inst, global_offset] : shifted)
    {
        inst->set_offset(global_offset);
    }
    // Mark off-plate instances as not printable for this plate
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            int lid = static_cast<int>(inst->loaded_id);
            bool on_plate = (identify_ids.find(lid) != identify_ids.end());
            inst->printable = on_plate;
            if (on_plate)
                inst->print_volume_state = ModelInstancePVS_Inside;
        }
    }

    if (has_partly_outside)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " has objects outside build volume, skipping";
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        return false;
    }
    return true;
}

bool SliceEngine::check_timeout(int plate_id)
{
    if (!m_has_timeout || std::chrono::steady_clock::now() <= m_timeout_deadline)
        return true;

    BOOST_LOG_TRIVIAL(error) << "Slicing timed out for plate " << (plate_id + 1);
    m_stats.issues.push_back(make_error(plate_id, "SLICING_TIMEOUT",
                                        "Slicing timed out. The model may be too complex. If you believe this is "
                                        "an error, please submit an appeal for review."));
    m_any_error = true;
    set_error_type(EXIT_SLICING_ERROR);
    return false;
}

void SliceEngine::init_print(Print& print)
{
    print.set_status_callback(
        [&print, this](const PrintBase::SlicingStatus& s)
        {
            default_status_callback(s, &print, &m_cfg.cancel_file);
        });
    print.is_BBL_printer() = m_preset_bundle->is_bbl_vendor();
}

void SliceEngine::assign_arrange_order()
{
    int order = 1;
    for (ModelObject* obj : m_model.objects)
        for (ModelInstance* inst : obj->instances)
            inst->arrange_order = order++;
}

void SliceEngine::setup_extruder_params(Print& print)
{
    int num_extruders = 0;
    if (m_config.has("filament_diameter"))
    {
        auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
        if (fd)
            num_extruders = static_cast<int>(fd->values.size());
    }
    Model::setExtruderParams(m_config, num_extruders);
    Model::setPrintSpeedTable(m_config, print.config());
}

void SliceEngine::check_spiral_lift_near_boundary(int plate_id, const BuildVolume& build_volume,
                                                  const std::set<int>& identify_ids)
{
    // Matches desktop 3DScene.cpp:1105-1122.
    bool spiral_lift_active = false;
    if (m_config.has("z_hop_types"))
    {
        auto* zht_opt = m_config.option<ConfigOptionEnumsGeneric>("z_hop_types");
        if (zht_opt)
        {
            for (int v : zht_opt->values)
            {
                if (v == static_cast<int>(ZHopType::zhtSpiral) || v == static_cast<int>(ZHopType::zhtAuto))
                {
                    spiral_lift_active = true;
                    break;
                }
            }
        }
    }
    if (!spiral_lift_active || build_volume.type() != BuildVolume_Type::Rectangle)
        return;

    constexpr double SPIRAL_LIFT_SAFETY_MARGIN = 3.5; // mm
    const BoundingBoxf3& bed_bb = build_volume.bounding_volume();
    std::set<std::string> warned_objects;
    for (ModelObject* obj : m_model.objects)
    {
        if (!obj->printable)
            continue;
        for (size_t idx = 0; idx < obj->instances.size(); ++idx)
        {
            ModelInstance* inst = obj->instances[idx];
            if (!inst->printable)
                continue;
            int lid = static_cast<int>(inst->loaded_id);
            if (identify_ids.find(lid) == identify_ids.end())
                continue;
            if (inst->print_volume_state != ModelInstancePVS_Inside)
                continue;
            BoundingBoxf3 bb = obj->instance_bounding_box(idx);
            double dist_left = std::abs(bb.min.x() - bed_bb.min.x());
            double dist_right = std::abs(bed_bb.max.x() - bb.max.x());
            double dist_bottom = std::abs(bb.min.y() - bed_bb.min.y());
            double dist_top = std::abs(bed_bb.max.y() - bb.max.y());
            double min_dist = std::min({dist_left, dist_right, dist_bottom, dist_top});
            if (min_dist < SPIRAL_LIFT_SAFETY_MARGIN
                && warned_objects.insert(obj->name).second)
            {
                m_stats.issues.push_back(
                    make_warning(plate_id, "SPIRAL_LIFT_NEAR_BOUNDARY",
                                 "Model too close to bed boundary. "
                                 "Disable spiral lifting or keep at least 3.5mm gap to avoid collision.",
                                 obj->name));
            }
        }
    }
}

bool SliceEngine::apply_model(int plate_id, Print& print, const Vec3d& origin)
{
    // plate_index from m_plate_data is already 0-based (import does -1 conversion)
    print.set_plate_index(plate_id);

    // Use the grid-layout-computed origin so object positions in gcode match
    // the desktop output (PartPlate::update_plate_layout_arrange).
    print.set_plate_origin(origin);

    DynamicPrintConfig merged_config = prepare_merged_config_for_plate(plate_id);
    print.apply(m_model, merged_config);

    if (print.num_object_instances() == 0)
    {
        m_stats.issues.push_back(
            make_warning(plate_id, "NO_PRINTABLE_OBJECTS", "No printable objects on this plate after apply"));
        return false;
    }

    return true;
}

DynamicPrintConfig SliceEngine::prepare_merged_config_for_plate(int plate_id)
{
    // Guard against wipe tower / tool change mismatch.
    // If the model uses fewer extruders than filaments configured in the 3MF,
    // the wipe tower generates a tool change sequence that doesn't match actual
    // extrusion, causing "append_tcr was asked to do a toolchange it didn't expect".
    //
    // We must check multiple sources to avoid false positives on multi-color models:
    //   1. vol->get_extruders()  — per-volume assignment + MMU painting
    //   2. plates_custom_gcodes  — AMS per-layer ToolChange entries
    //   3. single_extruder_multi_material — non-Bambu single-extruder multi-material
    // Only trim when ALL sources agree the model is single-extruder.
    //
    // Work on a per-plate copy so extruder-count trimming does not leak
    // into subsequent plates (m_config is shared across the pipeline).
    DynamicPrintConfig merged_config = m_config;

    // NOTE: Relies on filter_instances(plate_id) (called at process_plate:1533)
    // having already marked off-plate instances printable=false. The
    // is_printable() guard below is the per-plate filter — do not remove
    // without replacing it with an explicit plate_id check.
    std::set<int> used_extruders;
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            if (!inst->is_printable())
                continue;
            for (ModelVolume* vol : obj->volumes)
            {
                if (!vol->is_model_part())
                    continue;
                for (int eid : vol->get_extruders())
                    used_extruders.insert(eid);
            }
        }
    }

    int num_filaments = 0;
    if (m_config.has("filament_diameter"))
    {
        auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
        if (fd)
            num_filaments = static_cast<int>(fd->values.size());
    }

    // If volumes suggest single extruder, also check plate-level ToolChange
    // custom G-code (AMS per-layer filament switching).
    if (used_extruders.size() <= 1 && num_filaments > 1)
    {
        auto it = m_model.plates_custom_gcodes.find(plate_id);
        if (it != m_model.plates_custom_gcodes.end())
        {
            for (const auto& item : it->second.gcodes)
            {
                if (item.type == CustomGCode::Type::ToolChange && item.extruder > 0)
                    used_extruders.insert(item.extruder);
            }
        }
    }

    // If still single, also check the single_extruder_multi_material config flag
    // (used by non-Bambu printers for single-nozzle multi-filament). A model that
    // genuinely drives multiple filaments through one nozzle must NOT be trimmed,
    // or the wipe-tower tool-change sequence would no longer match actual extrusion.
    //
    // NOTE: Do not encode this "skip trim" decision by inserting sentinel extruder
    // IDs into `used_extruders` (an earlier revision inserted {1, 2} to force
    // size()==2). That collection carries real per-volume/AMS extruder statistics
    // and is read back as the 1-based keep index at line (*) below; seeding it
    // with fake values couples control flow to statistics and turns into a latent
    // bug the moment a later refactor intersects the set. Keep the decision in a
    // dedicated boolean instead.
    bool semm_multi_material = false;
    if (used_extruders.size() <= 1 && num_filaments > 1)
    {
        auto* semm = m_config.option<ConfigOptionBool>("single_extruder_multi_material");
        semm_multi_material = semm && semm->value;
    }

    if (!semm_multi_material && used_extruders.size() <= 1 && num_filaments > 1)
    {
        // Keep the filament data for the extruder actually used on this
        // plate, not just the first slot.  Without this remap, a plate
        // that only uses slot 1 (PETG) would inherit slot 0 (ABS) values
        // for filament_is_high_temperature, temperature_vitrification, etc.,
        // which causes wrong chamber cooling mode and other downstream bugs.
        int keep_idx = 0;
        if (used_extruders.size() == 1)
            keep_idx = *used_extruders.begin() - 1; // (line *) 1-based extruder id -> 0-based slot
        if (keep_idx < 0 || keep_idx >= num_filaments)
            keep_idx = 0;

        BOOST_LOG_TRIVIAL(info) << "Trimming filament config from " << num_filaments
                                << " to 1 (keeping slot " << keep_idx
                                << ") to match single-extruder model";

        trim_filament_config_to_single(merged_config, keep_idx);
    }
    else if (used_extruders.size() <= 1)
    {
        BOOST_LOG_TRIVIAL(info) << "Disabling prime tower (single extruder model)";
        merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    }

    // Apply per-plate config overrides (curr_bed_type, print_sequence, spiral_mode, etc.)
    for (const auto& pd : m_plate_data)
    {
        if (pd->plate_index == plate_id && !pd->config.empty())
        {
            merged_config.apply(pd->config);
            break;
        }
    }
    return merged_config;
}

void SliceEngine::trim_filament_config_to_single(DynamicPrintConfig& config, int keep_idx) const
{
    // Truncate all filament-related array options in `config` to a single
    // entry, preserving the values of slot `keep_idx` at index 0.  This
    // prevents Print::has_wipe_tower() from returning true due to
    // filament_diameter.size() > 1, which is the root cause of the
    // "append_tcr was asked to do a toolchange it didn't expect" error.
    // flush_volumes_matrix (N*N flat vector) and wiping_volumes_extruders
    // must also be trimmed to keep the config internally consistent:
    // a 5*5 matrix with only 1 extruder would mismatch sqrt(size) later.
    // Also disables the prime tower (no multi-extruder wipe needed).

    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

    constexpr const char* trim_keys[] = {
        "filament_diameter",            "filament_density",
        "filament_cost",                "filament_colour",
        "filament_type",                "filament_is_support",
        "filament_settings_id",         "nozzle_diameter",
        "flush_volumes_matrix",         "wiping_volumes_extruders",
        "filament_is_high_temperature", "temperature_vitrification",
    };

    // Generic remap: move slot keep_idx to index 0, then truncate to size 1.
    auto remap = [keep_idx](auto& vals) {
        if (keep_idx > 0 && keep_idx < static_cast<int>(vals.size()))
            vals[0] = vals[keep_idx];
        vals.resize(1);
    };

    for (const char* key : trim_keys)
    {
        auto* opt = config.option(key, true);
        if (!opt)
            continue;

        if (auto* fs = dynamic_cast<ConfigOptionFloats*>(opt))
        {
            if (!fs->values.empty())
            {
                remap(fs->values);
                config.set_key_value(key, new ConfigOptionFloats(fs->values));
            }
        }
        else if (auto* ss = dynamic_cast<ConfigOptionStrings*>(opt))
        {
            if (!ss->values.empty())
            {
                remap(ss->values);
                config.set_key_value(key, new ConfigOptionStrings(ss->values));
            }
        }
        else if (auto* bs = dynamic_cast<ConfigOptionBools*>(opt))
        {
            if (!bs->values.empty())
            {
                remap(bs->values);
                config.set_key_value(key, new ConfigOptionBools(bs->values));
            }
        }
        else if (auto* is_opt = dynamic_cast<ConfigOptionInts*>(opt))
        {
            if (!is_opt->values.empty())
            {
                remap(is_opt->values);
                config.set_key_value(key, new ConfigOptionInts(is_opt->values));
            }
        }
        else
        {
            // Unknown ConfigOption derivation: log so future additions to
            // trim_keys (or new option types) are not silently skipped.
            BOOST_LOG_TRIVIAL(warning) << "trim_filament_config_to_single: key \""
                                       << key << "\" has unsupported ConfigOption type ("
                                       << typeid(*opt).name() << "), skipped — trim incomplete";
        }
    }
}

bool SliceEngine::run_validation(int plate_id, Print& print)
{
    StringObjectException warning;
    StringObjectException err = print.validate(&warning, nullptr, nullptr);

    refine_bed_mismatch_message(warning);
    refine_bed_mismatch_message(err);

    if (!warning.string.empty())
        emit_validate_warning(plate_id, warning);

    if (!err.string.empty())
        if (!emit_validate_error(plate_id, err))
            return false;

    if (!check_print_by_object(plate_id, print))
        return false;

    check_filament_bed_rules(plate_id, print);

    if (!check_filament_temp_mixing(plate_id))
        return false;

    return true;
}

void SliceEngine::refine_bed_mismatch_message(StringObjectException& ex)
{
    // Replace extruder number with user-friendly filament name (desktop parity).
    if (ex.type != STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE
        || !m_preset_bundle
        || ex.params.size() < 3)
        return;

    int extruder_idx = 0;
    try {
        extruder_idx = std::stoi(ex.params[2]) - 1;
    } catch (...) {
        return;
    }
    if (extruder_idx < 0
        || extruder_idx >= static_cast<int>(m_preset_bundle->filament_presets.size()))
        return;

    std::string preset_name = m_preset_bundle->filament_presets[extruder_idx];
    std::string alias;
    if (auto it = m_preset_bundle->filaments.find_preset(preset_name, false)) {
        if (it->is_system) {
            alias = it->alias;
        } else {
            auto* base = m_preset_bundle->filaments.get_preset_base(*it);
            if (base && !base->alias.empty())
                alias = base->alias;
        }
    }
    if (alias.empty()) {
        auto pos = preset_name.rfind(" @");
        if (pos != std::string::npos)
            alias = preset_name.substr(0, pos);
        else
            alias = preset_name;
    }
    ex.string = std::string("Plate ") + ex.params[0] + ": "
        + ex.params[1] + " is not suggested to be used to print filament "
        + ex.params[2] + " (" + alias + "). "
        + "If you still want to do this print job, please set this filament's "
        + "bed temperature to non-zero.";
}

void SliceEngine::emit_validate_warning(int plate_id, const StringObjectException& warning)
{
    auto [obj_name, opt_hint] = format_exception_context(warning);
    BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << ": " << warning.string << obj_name << opt_hint;

    std::string wcode;
    switch (warning.type)
    {
    case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE:
        wcode = "PRINT_VALIDATE_WARNING_FILAMENT_BED_MISMATCH";
        break;
    case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP:
        wcode = "PRINT_VALIDATE_WARNING_FILAMENT_TEMP_MISMATCH";
        break;
    case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT:
        wcode = "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_SEQ";
        break;
    case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT:
        wcode = "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_LAYER";
        break;
    case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT:
        wcode = "PRINT_VALIDATE_WARNING_LAYER_HEIGHT_LIMIT";
        break;
    case STRING_EXCEPT_ORGANIC_SUPPORT_VARIABLE_LAYER:
        m_stats.issues.push_back(make_error(plate_id, "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT",
            "Organic supports do not support variable layer height. "
            "Please disable variable layer height or switch to non-organic support type.",
            obj_name,
            "In Snapmaker Orca, disable variable layer height or change support type to default (non-organic)."));
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        break;
    default:
        wcode = "PRINT_VALIDATE_WARNING";
        break;
    }
    if (!wcode.empty())
        m_stats.issues.push_back(make_warning(plate_id, wcode, warning.string + opt_hint, obj_name));
}

bool SliceEngine::emit_validate_error(int plate_id, const StringObjectException& err)
{
    auto [obj_name, opt_hint] = format_exception_context(err);
    // STRING_EXCEPT_NOT_DEFINED (type 0) is used by the library for generic
    // checks (e.g. exceeds-build-volume-height) that the desktop GUI treats
    // as non-fatal warnings. Match that behaviour.
    if (err.type == STRING_EXCEPT_NOT_DEFINED)
    {
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;
        if (err.string.find("Variable layer height is not supported with Organic supports") != std::string::npos)
        {
            m_stats.issues.push_back(make_error(plate_id, "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT",
                "Organic supports do not support variable layer height. "
                "Please disable variable layer height or switch to non-organic support type.",
                obj_name,
                "In Snapmaker Orca, disable variable layer height or change support type to default (non-organic)."));
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
        }
        else
        {
            m_stats.issues.push_back(make_warning(plate_id, "PRINT_VALIDATE_WARNING",
                                                  err.string + opt_hint, obj_name));
        }
        // NOT_DEFINED never aborts slicing — falls through to subsequent checks.
        return true;
    }

    BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;
    std::string ecode;
    switch (err.type)
    {
    case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE:
        ecode = "PRINT_VALIDATE_FILAMENT_BED_MISMATCH";
        break;
    case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP:
        ecode = "PRINT_VALIDATE_FILAMENT_TEMP_MISMATCH";
        break;
    case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT:
        ecode = "PRINT_VALIDATE_OBJECT_COLLISION_SEQ";
        break;
    case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT:
        ecode = "PRINT_VALIDATE_OBJECT_COLLISION_LAYER";
        break;
    case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT:
        ecode = "PRINT_VALIDATE_LAYER_HEIGHT_LIMIT";
        break;
    case STRING_EXCEPT_ORGANIC_SUPPORT_VARIABLE_LAYER:
        ecode = "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT";
        break;
    default:
        ecode = "PRINT_VALIDATE_ERROR";
        break;
    }
    m_stats.issues.push_back(make_error(plate_id, ecode, err.string + opt_hint, obj_name));
    m_any_error = true;
    set_error_type(EXIT_PREPROCESS_ERROR);
    return false;
}

bool SliceEngine::check_print_by_object(int plate_id, Print& print)
{
    // U1 toolchanger cannot safely print by object — collision risk during
    // tool switches. Block at validation rather than letting it reach slicing.
    if (print.config().print_sequence.value != PrintSequence::ByObject)
        return true;

    m_stats.issues.push_back(make_error(
        plate_id, "PRINT_BY_OBJECT_CAUTION",
        "Print-by-object may cause the print head to collide with printed parts during switching.",
        "" /*object_name*/,
        "In Snapmaker Orca, switch to print-by-layer or ensure sufficient clearance between objects."));
    m_any_error = true;
    set_error_type(EXIT_PREPROCESS_ERROR);
    return false;
}

void SliceEngine::check_filament_bed_rules(int plate_id, Print& print)
{
    NozzleFilamentRuleMismatch nozzle_mismatch;
    bool is_gesp = false, is_pei_not_pla = false, is_pei_tpu = false;

    print.filament_rule_mismatch_flags(nozzle_mismatch, is_gesp,
                                       is_pei_not_pla, is_pei_tpu,
                                       m_preset_bundle.get());

    if (nozzle_mismatch.has_mismatch) {
        std::string msg = "Using a " + nozzle_mismatch.nozzle_diameter_mm
                        + " mm " + nozzle_mismatch.nozzle_type_key
                        + " nozzle for "
                        + (nozzle_mismatch.filament_preset_name.empty()
                               ? "(unknown)"
                               : nozzle_mismatch.filament_preset_name)
                        + " is not recommended.";
        m_stats.issues.push_back(make_warning(
            plate_id, "FILAMENT_NOZZLE_MISMATCH", msg));
    }

    // PEI + TPU has higher priority than PEI + non-PLA (desktop parity).
    if (is_pei_tpu) {
        m_stats.issues.push_back(make_warning(
            plate_id, "FILAMENT_BED_PEI_TPU_STICKING",
            "Filament may stick too strongly to the smooth PEI plate. "
            "Apply glue to protect the plate and ease part removal."));
    } else if (is_pei_not_pla) {
        m_stats.issues.push_back(make_warning(
            plate_id, "FILAMENT_BED_PEI_ADHESION",
            "Filament may not adhere well to the smooth PEI plate on "
            "the first layer. Apply glue before printing."));
    }

    if (is_gesp) {
        m_stats.issues.push_back(make_warning(
            plate_id, "FILAMENT_BED_GESP_ADHESION",
            "Low adhesion to the graphic effect plate may cause failure. "
            "Use a different filament instead."));
    }
}

bool SliceEngine::check_filament_temp_mixing(int plate_id)
{
    // Collect filament slots actually used by this plate, then check for
    // high/low temperature mixing via the UNTRIMMED global config (m_config).
    // This mirrors the desktop Plater::check_filament_temp_mixing() pattern
    // but keeps the binary high/low classification (no tri-state compatible
    // mode). Reading from m_config avoids the trimmed print.config() which
    // has already lost multi-extruder information via prepare_merged_config.
    int num_filaments = 0;
    auto* ftype_opt = m_config.option<ConfigOptionStrings>("filament_type");
    if (ftype_opt)
        num_filaments = static_cast<int>(ftype_opt->values.size());
    if (num_filaments <= 1)
        return true;

    std::set<int> used_slots;

    // (a) Collect extruders from printable model instances on this plate.
    // Also track whether any object relies on the global default extruder —
    // when it does, per-feature defaults (wall / infill filaments) affect
    // the slicing output and must be collected.
    //
    // NOTE: Per-plate filtering relies on filter_instances(plate_id) at
    // process_plate:1533 having marked off-plate instances printable=false.
    // The is_printable() guard inside this loop is the per-plate filter —
    // equivalent to desktop model_object_is_on_plate().
    bool uses_default_extruder = false;
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            if (!inst->is_printable())
                continue;
            for (ModelVolume* vol : obj->volumes)
            {
                if (!vol->is_model_part())
                    continue;
                for (int eid : vol->get_extruders())
                {
                    if (eid >= 1 && eid <= num_filaments)
                        used_slots.insert(eid - 1);
                }
            }
            // An object without an explicit "extruder" key in its config
            // (or with extruder==0) inherits the global default.  This
            // matches the desktop Plater::check_filament_temp_mixing logic.
            if (!obj->config.has("extruder") || obj->config.extruder() == 0)
                uses_default_extruder = true;
        }
    }

    // (b) Collect global feature extruders from the untrimmed config.
    // Always-collected keys: wipe tower and support filaments apply
    // regardless of object-level overrides.
    {
        static const std::vector<const char*> always_keys = {
            "wipe_tower_filament",
            "support_filament",
            "support_interface_filament"
        };
        for (const char* key : always_keys)
        {
            auto* opt = m_config.option<ConfigOptionInt>(key);
            if (opt && opt->value >= 1 && opt->value <= num_filaments)
                used_slots.insert(opt->value - 1);
        }
    }

    // (c) When at least one object uses the default extruder, the global
    // wall / infill defaults affect the output and must be collected.
    if (uses_default_extruder)
    {
        static const std::vector<const char*> default_keys = {
            "wall_filament",
            "sparse_infill_filament",
            "solid_infill_filament"
        };
        for (const char* key : default_keys)
        {
            auto* opt = m_config.option<ConfigOptionInt>(key);
            if (opt && opt->value >= 1 && opt->value <= num_filaments)
                used_slots.insert(opt->value - 1);
        }

        // Resolve the global default extruder itself.
        auto* extruder_opt = m_config.option<ConfigOptionInt>("extruder");
        if (extruder_opt && extruder_opt->value >= 1 && extruder_opt->value <= num_filaments)
            used_slots.insert(extruder_opt->value - 1);
    }

    if (used_slots.empty())
        return true;

    // Check high/low mixing across the actually-used slots.
    auto* f_i_h_t = m_config.option<ConfigOptionBools>("filament_is_high_temperature");
    if (!f_i_h_t)
        return true;

    bool has_high = false, has_low = false;
    for (int slot : used_slots)
    {
        if (slot >= static_cast<int>(f_i_h_t->values.size()))
            continue;
        if (f_i_h_t->values[slot])
            has_high = true;
        else
            has_low = true;
    }

    if (!has_high || !has_low)
        return true;

    std::string msg =
        "Cannot print multiple filaments which have large difference of "
        "temperature together. Otherwise, the extruder and nozzle may be "
        "blocked or damaged during printing.";
    BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": " << msg;
    m_stats.issues.push_back(make_error(plate_id, "FILAMENT_TEMP_MIXING", msg));
    m_any_error = true;
    set_error_type(EXIT_PREPROCESS_ERROR);
    return false;
}

bool SliceEngine::run_slicing(int plate_id, Print& print)
{
    BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << plate_id << "...";

    try
    {
        print.process();
    }
    catch (const SlicingErrors& exs)
    {
        for (const auto& ex : exs.errors_)
        {
            std::string msg = ex.what();
            BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << msg;
            m_stats.issues.push_back(make_error(plate_id, "SLICING_ERROR",
                                                "Slicing failed for this plate: " + msg));
        }
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const SlicingError& ex)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << ex.what();
        m_stats.issues.push_back(make_error(
            plate_id, "SLICING_FATAL_ERROR",
            "A fatal slicing error occurred while processing this plate. "
            "The model geometry or print settings may be incompatible with the selected configuration."));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const CanceledException&)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": Slicing was cancelled.";
        m_stats.issues.push_back(make_error(plate_id, "SLICING_CANCELLED",
                                            "Slicing was cancelled before completion. "
                                            "This may be caused by an external cancellation request or a resource limit."));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing exception: " << e.what();
        m_stats.issues.push_back(
            make_error(plate_id, "SLICING_EXCEPTION",
                       "An unexpected internal error interrupted slicing for this plate. "
                       "This is not caused by the model — please try again or contact support."));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }

    return true;
}

bool SliceEngine::export_gcode(int plate_id, Print& print, PlateSliceResult& result)
{
    std::string gcode_output;
    if (m_cfg.format == OutputFormat::GCODE_3MF || !m_cfg.single_plate)
    {
        gcode_output = m_cfg.temp_dir + "/plate_" + std::to_string(plate_id) + ".gcode";
        m_temp_files.push_back(gcode_output);
    }
    else
    {
        gcode_output = m_output_path;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code for plate " << plate_id << "...";

    try
    {
        GCodeProcessor::s_IsBBLPrinter = print.is_BBL_printer();

        auto thumbnail_cb = [this](const Slic3r::ThumbnailsParams& params) -> std::vector<Slic3r::ThumbnailData>
        {
            std::vector<Slic3r::ThumbnailData> thumbnails;
            const Slic3r::ThumbnailData* source = nullptr;
            for (const auto& pd : m_plate_data)
            {
                if (pd->plate_index == params.plate_id && pd->plate_thumbnail.is_valid())
                {
                    source = &pd->plate_thumbnail;
                    break;
                }
            }
            if (!source)
                return thumbnails;
            for (const auto& size : params.sizes)
                thumbnails.push_back(resize_thumbnail(*source, static_cast<unsigned int>(size.x()),
                                                      static_cast<unsigned int>(size.y())));
            return thumbnails;
        };

        std::string exported = print.export_gcode(gcode_output, &result.gcode_result, thumbnail_cb);
        result.gcode_path = exported;

        // Capture the post-trim filament colours the slicer actually used (print.config()
        // reflects the merged_config after apply, including any single-extruder remap).
        // assemble_plate_stats reads these so the reported colour matches the G-code header
        // instead of the raw, pre-trim m_config (whose index 0 may point at a different slot).
        if (print.config().has("filament_colour")) {
            const auto* fc = print.config().option<ConfigOptionStrings>("filament_colour");
            if (fc) result.filament_colours = fc->values;
        }
        // Capture the post-trim filament types for the same reason as colours above:
        // the slicer applies the merged_config after single-extruder remap, so print.config()
        // holds the type array indexed the way the G-code header is, unlike the raw m_config.
        if (print.config().has("filament_type")) {
            const auto* ft = print.config().option<ConfigOptionStrings>("filament_type");
            if (ft) result.filament_types = ft->values;
        }
        // Capture post-trim nozzle / filament diameters / densities so downstream stats
        // compute length/weight against the values the slicer actually used, not the raw
        // pre-trim m_config (whose index 0 may point at a different slot after single-extruder
        // remap). Same rationale as colours/types above.
        if (print.config().has("nozzle_diameter")) {
            const auto* nd = print.config().option<ConfigOptionFloats>("nozzle_diameter");
            if (nd) result.nozzle_diameters = nd->values;
        }
        if (print.config().has("filament_diameter")) {
            const auto* fd = print.config().option<ConfigOptionFloats>("filament_diameter");
            if (fd) result.filament_diameters = fd->values;
        }
        if (print.config().has("filament_density")) {
            const auto* fden = print.config().option<ConfigOptionFloats>("filament_density");
            if (fden) result.filament_densities = fden->values;
        }

        // Post-processing scripts are disabled in cloud mode to prevent
        // remote code execution via user-uploaded 3MF files.
        // run_post_process_scripts(result.gcode_path, print.full_print_config());

        // Collect PrintBase warnings with message_id-aware grading.
        // Desktop CLI treats EmptyGcodeLayers and GcodeOverlap as
        // CLI_SLICING_ERROR (hard exit). Cloud engine flags the plate
        // but continues processing remaining plates.
        auto grade_warning = [&](const PrintStateBase::Warning& w)
        {
            if (!w.current)
                return;
            auto msg_type = static_cast<PrintStateBase::SlicingNotificationType>(w.message_id);
            if (msg_type == PrintStateBase::SlicingEmptyGcodeLayers)
            {
                result.issues.push_back(make_error(plate_id, "PRINT_EMPTY_GCODE_LAYERS",
                                                   "Empty G-code layers detected: " + w.message));
                m_any_error = true;
                set_error_type(EXIT_POSTPROCESS_ERROR);
            }
            else if (msg_type == PrintStateBase::SlicingGcodeOverlap)
            {
                result.issues.push_back(make_warning(plate_id, "PRINT_GCODE_OVERLAP",
                                                     "G-code overlap detected: " + w.message));
                m_any_postprocess_warning = true;
            }
            else if (w.level == PrintStateBase::WarningLevel::CRITICAL)
            {
                result.issues.push_back(make_warning(plate_id, "PRINT_WARNING_CRITICAL", w.message));
            }
            else
            {
                result.issues.push_back(make_warning(plate_id, "PRINT_WARNING", w.message));
            }
        };

        for (int step = 0; step < static_cast<int>(PrintStep::psCount); ++step)
        {
            const auto& wstate = print.step_state_with_warnings(static_cast<PrintStep>(step));
            for (const auto& w : wstate.warnings)
                grade_warning(w);
        }

        for (const PrintObject* obj : print.objects())
        {
            for (int step = 0; step < static_cast<int>(PrintObjectStep::posCount); ++step)
            {
                const auto& wstate = obj->step_state_with_warnings(static_cast<PrintObjectStep>(step));
                for (const auto& w : wstate.warnings)
                    grade_warning(w);
            }
        }

        const PrintStatistics& ps = print.print_statistics();
        result.total_weight = ps.total_weight;
        result.support_used = print.is_support_used();
        result.total_used_filament = ps.total_used_filament;
        result.total_cost = ps.total_cost;
        result.filament_volumes = result.gcode_result.print_statistics.total_volumes_per_extruder;

        return true;
    }
    catch (const std::exception& e)
    {
        // Check for wipe tower tool change mismatch before generic reporting.
        // CGAL/float differences on some platforms cause non-consecutive
        // extruder ID handling to fail during G-code export.
        if (is_wipe_tower_error(result))
        {
            BOOST_LOG_TRIVIAL(error) << "Wipe tower tool change mismatch on plate " << (plate_id + 1)
                                     << ". This model may be incompatible with the "
                                        "prime tower. Please disable the prime tower and re-submit.";
            m_stats.issues.push_back(make_error(plate_id, "WIPE_TOWER_TOOLCHANGE_ERROR",
                                                "Wipe tower tool change mismatch. Please disable the prime tower "
                                                "in filament settings and re-submit the model."));
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << plate_id << ": " << e.what();
        }
        result.issues.push_back(
            make_error(plate_id, "GCODE_EXPORT_ERROR",
                       "Failed to write G-code output. "
                       "Check that the output directory exists and has sufficient disk space."));
        m_any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        m_plate_results[plate_id] = result;
        return false;
    }
}

bool SliceEngine::check_empty_gcode_layers(int plate_id, PlateSliceResult& slice_result)
{
    // EmptyGcodeLayers means the plate has no valid layers; the G-code file
    // exists but is effectively empty. Skip post-processing and flag as failed.
    bool has_empty_gcode_layers = false;
    for (const auto& iss : slice_result.issues)
    {
        if (iss.code == "PRINT_EMPTY_GCODE_LAYERS")
        {
            has_empty_gcode_layers = true;
            break;
        }
    }
    if (!has_empty_gcode_layers)
        return true;

    boost::filesystem::remove(slice_result.gcode_path);
    BOOST_LOG_TRIVIAL(warning) << "Plate " << (plate_id + 1)
                               << ": empty G-code layers, G-code file discarded";
    for (auto& iss : slice_result.issues)
        m_stats.issues.push_back(std::move(iss));
    slice_result.issues.clear();
    slice_result.gcode_result.moves.clear();
    slice_result.gcode_result.moves.shrink_to_fit();
    slice_result.gcode_result.lines_ends.clear();
    slice_result.gcode_result.lines_ends.shrink_to_fit();
    m_plate_results[plate_id] = slice_result;
    return false;
}

void SliceEngine::do_postprocessing(int plate_id, PlateSliceResult& result)
{
    bool has_postprocess_error = false;
    bool has_postprocess_warning = false;

    // Toolpaths outside print volume. Desktop blocks printing via
    // is_slice_result_ready_for_print() when toolpath_outside is true.
    if (result.gcode_result.toolpath_outside)
    {
        log_plate_message("[Post-processing]", "ERROR", plate_id,
                          "Toolpaths extend outside the printable area.");
        has_postprocess_error = true;
        result.issues.push_back(make_error(plate_id, "TOOLPATH_OUTSIDE",
                                           "Toolpaths extend outside the printable area. "
                                           "The object exceeds the printer's build volume and cannot be printed safely."));
    }

    // Tool height outside check (matches desktop GLCanvas3D.cpp:9658)
    if (!result.gcode_result.moves.empty() && result.gcode_result.printable_height > 0.0f)
    {
        float max_z = result.gcode_result.moves[0].position.z();
        for (const auto& move : result.gcode_result.moves)
            if (move.position.z() > max_z)
                max_z = move.position.z();
        if (max_z - result.gcode_result.printable_height >= 1e-6)
        {
            log_plate_message("[Post-processing]", "WARNING", plate_id,
                              "A G-code path goes beyond the max print height.");
            has_postprocess_warning = true;
            Issue h = make_warning(plate_id, "TOOL_HEIGHT_OUTSIDE",
                                   "A G-code path goes beyond the max print height. "
                                   "The object may not print correctly.");
            h.z_height = static_cast<double>(max_z);
            result.issues.push_back(h);
        }
    }

    // Toolpath conflict detection
    if (result.gcode_result.conflict_result.has_value())
    {
        const auto& cr = result.gcode_result.conflict_result.value();

        // Build sorted, unique list of layer print-Z values from extrusion moves,
        // replicating the desktop GCodeViewer::load logic (GCodeViewer.cpp:3011-3018).
        // Only EMoveType::Extrude moves define layer boundaries, deduplicated within
        // EPSILON tolerance (libslic3r.h:52: static constexpr double EPSILON = 1e-4).
        std::vector<double> layer_zs;
        {
            constexpr double LAYER_Z_EPSILON = 1e-4;
            for (const auto& move : result.gcode_result.moves)
            {
                if (move.type != EMoveType::Extrude)
                    continue;
                const double z = static_cast<double>(move.position.z());
                if (layer_zs.empty() || z < layer_zs.back() - LAYER_Z_EPSILON || layer_zs.back() + LAYER_Z_EPSILON < z)
                    layer_zs.push_back(z);
            }
        }

        // Compute layer number matching desktop's Layers::get_l_at (GCodeViewer.hpp:492-496)
        int computed_layer = 0;
        int total_layers = static_cast<int>(layer_zs.size());
        if (!layer_zs.empty())
        {
            auto iter = std::upper_bound(layer_zs.begin(), layer_zs.end(), cr._height);
            computed_layer = static_cast<int>(std::distance(layer_zs.begin(), iter));
        }

        std::string obj1 = cr._obj1 ? cr._objName1 : "Wipe Tower";
        std::string obj2 = cr._obj2 ? cr._objName2 : "Wipe Tower";
        std::string conflict_msg = "Conflicts of G-code paths have been found at layer "
            + std::to_string(computed_layer) + "/" + std::to_string(total_layers)
            + ", z = " + std::to_string(cr._height) + " mm."
            + " Please separate the conflicted objects farther ("
            + obj1 + " <-> " + obj2 + ").";
        log_plate_message("[Post-processing]", "WARNING", plate_id, conflict_msg);
        has_postprocess_warning = true;
        m_any_error = true;
        set_error_type(EXIT_POSTPROCESS_ERROR);
        Issue conflict = make_serious_warning(plate_id, "TOOLPATH_CONFLICT",
                                    conflict_msg,
                                    obj1 + " vs " + obj2);
        conflict.z_height = cr._height;
        conflict.layer = computed_layer;
        result.issues.push_back(conflict);
    }

    // Bed/filament compatibility
    if (!result.gcode_result.bed_match_result.match)
    {
        const auto& bm = result.gcode_result.bed_match_result;
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "BED_FILAMENT_MISMATCH",
                                             "Filament " + std::to_string(bm.extruder_id + 1) +
                                                 " is not compatible with bed type \"" + bm.bed_type_name + "\""));
    }

    // Timelapse warnings
    if (result.gcode_result.timelapse_warning_code & 1)
    {
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "TIMELAPSE_SPIRAL_VASE",
                                             "Timelapse is not supported in spiral vase mode on this printer"));
    }
    if ((result.gcode_result.timelapse_warning_code >> 1) & 1)
    {
        has_postprocess_warning = true;
        result.issues.push_back(
            make_warning(plate_id, "TIMELAPSE_BY_OBJECT",
                         "Timelapse is not supported with by-object print sequence on this printer"));
    }

    // Slice warnings
    for (const auto& w : result.gcode_result.warnings)
    {
        if (w.error_code == "1000C001")
            continue; // bed temp warning irrelevant for cloud slicing
        if (w.level >= 2)
        {
            log_plate_message("[Post-processing]", "ERROR", plate_id, w.msg + " (code: " + w.error_code + ")");
            has_postprocess_error = true;
            result.issues.push_back(make_error(plate_id, w.error_code, w.msg + " (code: " + w.error_code + ")"));
        }
        else if (w.level == 1)
        {
            has_postprocess_warning = true;
            result.issues.push_back(make_warning(plate_id, w.error_code, w.msg + " (code: " + w.error_code + ")"));
        }
        else
        {
            result.issues.push_back(make_tip(plate_id, w.error_code, w.msg));
        }
    }

    result.has_postprocess_warning = has_postprocess_warning;
    if (has_postprocess_warning)
        m_any_postprocess_warning = true;
    if (has_postprocess_error)
    {
        m_any_error = true;
        set_error_type(EXIT_POSTPROCESS_ERROR);
    }
}

void SliceEngine::finalise_plate_result(int plate_id, PlateSliceResult& result)
{
    // Free G-code visualization data that the cloud engine never uses.
    // GCodeProcessorResult::moves and lines_ends store every G-code move
    // vertex for the desktop GUI — hundreds of MB per plate.  Retaining
    // them across plates causes std::bad_alloc on complex multi-plate
    // projects (e.g. Mochi Makes plate 3 with 10 instances after plates
    // 1+2 already consumed 500+ MB for their moves vectors).
    result.gcode_result.moves.clear();
    result.gcode_result.moves.shrink_to_fit();
    result.gcode_result.lines_ends.clear();
    result.gcode_result.lines_ends.shrink_to_fit();

    m_plate_results[plate_id] = result;
}

// ============================================================================
// Stage 5: Package output as gcode.3mf
// ============================================================================

void SliceEngine::package_output()
{
    BOOST_LOG_TRIVIAL(info) << "Creating gcode.3mf package...";

    std::string printer_model_id;
    if (m_config.has("printer_model"))
        printer_model_id = m_config.opt_string("printer_model");

    populate_plate_data_for_export(printer_model_id);

    StoreParams params;
    params.path = m_output_path.c_str();
    params.plate_data_list = m_plate_data;
    params.model = &m_model;
    params.config = &m_config;
    params.project_presets = m_project_presets;

    if (m_cfg.single_plate)
        params.export_plate_idx = m_cfg.plate_id - 1;
    else
        params.export_plate_idx = -1;

    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel |
                      SaveStrategy::WithGcode | SaveStrategy::SkipModel |
                      SaveStrategy::Zip64;
    params.project = nullptr;
    params.profile = nullptr;

    // Thumbnail data: one default-constructed (reset) ThumbnailData per plate,
    // every pointer non-NULL but is_valid() == false.  Prevents NULL deref in
    // headless environments (no GPU / Mesa / EGL) while the fallback path in
    // _BBS_3MF_Exporter still picks up PNG files from plate_data on disk.
    // Owned holders must outlive the store_bbs_3mf call — params holds raw
    // pointers into them.
    size_t plate_count = m_plate_data.size();
    std::vector<std::unique_ptr<ThumbnailData>> thumbnail_owned;
    std::vector<std::unique_ptr<ThumbnailData>> no_light_thumbnail_owned;
    std::vector<std::unique_ptr<ThumbnailData>> top_thumbnail_owned;
    std::vector<std::unique_ptr<ThumbnailData>> pick_thumbnail_owned;
    std::vector<std::unique_ptr<ThumbnailData>> calibration_thumbnail_owned;
    thumbnail_owned.reserve(plate_count);
    no_light_thumbnail_owned.reserve(plate_count);
    top_thumbnail_owned.reserve(plate_count);
    pick_thumbnail_owned.reserve(plate_count);
    calibration_thumbnail_owned.reserve(plate_count);
    for (size_t i = 0; i < plate_count; ++i)
    {
        thumbnail_owned.push_back(std::make_unique<ThumbnailData>());
        no_light_thumbnail_owned.push_back(std::make_unique<ThumbnailData>());
        top_thumbnail_owned.push_back(std::make_unique<ThumbnailData>());
        pick_thumbnail_owned.push_back(std::make_unique<ThumbnailData>());
        calibration_thumbnail_owned.push_back(std::make_unique<ThumbnailData>());
        params.thumbnail_data.push_back(thumbnail_owned.back().get());
        params.no_light_thumbnail_data.push_back(no_light_thumbnail_owned.back().get());
        params.top_thumbnail_data.push_back(top_thumbnail_owned.back().get());
        params.pick_thumbnail_data.push_back(pick_thumbnail_owned.back().get());
        params.calibration_thumbnail_data.push_back(calibration_thumbnail_owned.back().get());
    }

    std::vector<PlateBBoxData*> id_bboxes;
    std::vector<std::unique_ptr<PlateBBoxData>> id_bboxes_owned;
    id_bboxes_owned.reserve(plate_count);
    for (size_t i = 0; i < plate_count; ++i)
    {
        id_bboxes_owned.push_back(std::make_unique<PlateBBoxData>());
        id_bboxes.push_back(id_bboxes_owned.back().get());
    }
    params.id_bboxes = id_bboxes;

    try
    {
        bool success = store_bbs_3mf(params);
        if (!success)
        {
            BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package";
            m_any_error = true;
            set_error_type(EXIT_EXPORT_ERROR);
            std::string msg = "Failed to create output package. The slicing result could not be saved.";
            m_stats.issues.push_back(make_error(-1, "PACKAGE_EXPORT_ERROR", msg));
            if (m_stats.error_message.empty())
                m_stats.error_message = msg;
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "gcode.3mf package created: " << m_output_path;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
        m_any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        std::string msg = "Failed to create output package due to an internal error.";
        m_stats.issues.push_back(make_error(-1, "PACKAGE_EXPORT_ERROR", msg));
        if (m_stats.error_message.empty())
            m_stats.error_message = msg;
    }
}

void SliceEngine::populate_plate_data_for_export(const std::string& printer_model_id)
{
    const ConfigOptionStrings* filament_types =
        m_config.has("filament_type") ? m_config.option<ConfigOptionStrings>("filament_type") : nullptr;
    const ConfigOptionStrings* filament_colors =
        m_config.has("filament_colour") ? m_config.option<ConfigOptionStrings>("filament_colour") : nullptr;
    const ConfigOptionStrings* filament_ids =
        m_config.has("filament_ids") ? m_config.option<ConfigOptionStrings>("filament_ids") : nullptr;

    // Fallback nozzle string from the raw m_config (N slots) for plates whose
    // snapshot wasn't populated (e.g. failed before export_gcode).
    std::string fallback_nozzle_str;
    if (m_config.has("nozzle_diameter"))
    {
        auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt && !nozzle_opt->values.empty())
        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2);
            for (size_t i = 0; i < nozzle_opt->values.size(); ++i)
            {
                if (i > 0) ss << ",";
                ss << nozzle_opt->values[i];
            }
            fallback_nozzle_str = ss.str();
        }
    }

    auto join_nozzles = [](const std::vector<double>& vals) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        for (size_t i = 0; i < vals.size(); ++i)
        {
            if (i > 0) ss << ",";
            ss << vals[i];
        }
        return ss.str();
    };

    for (auto& pd : m_plate_data)
    {
        auto it = m_plate_results.find(pd->plate_index);
        if (it == m_plate_results.end()) continue;

        PlateSliceResult& result = it->second;
        pd->gcode_file = result.gcode_path;
        pd->is_sliced_valid = true;
        pd->printer_model_id = printer_model_id;
        // Per-plate nozzle string: prefer the post-trim snapshot (matches the single value
        // the slicer actually used on this plate), fall back to the raw m_config N-slot string.
        pd->nozzle_diameters = !result.nozzle_diameters.empty()
                                   ? join_nozzles(result.nozzle_diameters)
                                   : fallback_nozzle_str;

        auto& modes = result.gcode_result.print_statistics.modes;
        int print_time = static_cast<int>(modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
        pd->gcode_prediction = std::to_string(print_time);

        if (result.total_weight != 0.0)
        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << result.total_weight;
            pd->gcode_weight = ss.str();
        }

        pd->toolpath_outside = result.gcode_result.toolpath_outside;
        pd->timelapse_warning_code = result.gcode_result.timelapse_warning_code;
        pd->is_support_used = result.support_used;
        pd->is_label_object_enabled = result.gcode_result.label_object_enabled;

        pd->parse_filament_info(&result.gcode_result);

        for (auto& info : pd->slice_filaments_info)
        {
            size_t idx = static_cast<size_t>(info.id);
            // Prefer the post-trim types captured at slice time (result.filament_types),
            // mirroring the colour handling above. m_config is the raw pre-trim array, whose
            // index 0 may point at a different slot after single-extruder remap.
            if (idx < result.filament_types.size())
                info.type = result.filament_types[idx];
            else if (filament_types && idx < filament_types->values.size())
                info.type = filament_types->values[idx];
            // Use the post-trim colours captured at slice time (result.filament_colours),
            // which match the G-code header. m_config holds the raw pre-trim array, whose
            // index 0 may point at a different slot after single-extruder remap.
            if (idx < result.filament_colours.size())
                info.color = result.filament_colours[idx];
            else if (filament_colors && idx < filament_colors->values.size())
                info.color = filament_colors->values[idx];
            if (filament_ids && idx < filament_ids->values.size())
                info.filament_id = filament_ids->values[idx];
        }

        // Rebuild objects_and_instances using model.objects array indices
        std::set<int> plate_identify_ids;
        for (const auto& entry : pd->obj_inst_map)
            plate_identify_ids.insert(entry.second.second);

        pd->objects_and_instances.clear();
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx)
        {
            const ModelObject* obj = m_model.objects[obj_idx];
            for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx)
            {
                const ModelInstance* inst = obj->instances[inst_idx];
                if (plate_identify_ids.count(static_cast<int>(inst->loaded_id)))
                    pd->objects_and_instances.emplace_back(
                        static_cast<int>(obj_idx),
                        static_cast<int>(inst_idx));
            }
        }
    }
}

// ============================================================================
// Exit code derivation
// ============================================================================

void SliceEngine::set_error_type(int code)
{
    if (code > m_error_type)
        m_error_type = code;
}

int SliceEngine::exit_code() const
{
    if (m_error_type > EXIT_OK)
        return m_error_type;
    if (m_any_error)
        return EXIT_PREPROCESS_ERROR;
    // Post-processing warnings are non-fatal — G-code has been generated
    // and is usable. Treat as success per alignment with desktop behavior.
    if (m_any_postprocess_warning)
        return EXIT_OK;
    return EXIT_OK;
}

// ============================================================================
// Stage 6: Build statistics for JSON output
// ============================================================================

void SliceEngine::build_statistics()
{
    for (const auto& [plate_id, result] : m_plate_results)
    {
        SliceOutputStats::PlateStats plate_stats;
        plate_stats.plate_id = plate_id;
        assemble_plate_stats(plate_id, result, plate_stats);
        m_stats.plates.push_back(plate_stats);
    }

    patch_orphan_plate_issues();
    finalise_statistics();
}

void SliceEngine::assemble_plate_stats(int plate_id, const PlateSliceResult& result,
                                       SliceOutputStats::PlateStats& plate_stats)
{
    bool plate_has_error = false;
    bool plate_has_warning = false;
    for (const auto& issue : result.issues)
    {
        if (issue.level == IssueLevel::error || issue.level == IssueLevel::serious_warning)
            plate_has_error = true;
        if (issue.level == IssueLevel::warning || issue.level == IssueLevel::serious_warning)
            plate_has_warning = true;
        m_stats.issues.push_back(issue);
    }

    plate_stats.success = !plate_has_error;
    if (plate_has_error)
    {
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
    }
    if (plate_has_warning || result.has_postprocess_warning)
        m_any_postprocess_warning = true;

    plate_stats.issues = result.issues;
    plate_stats.plate_count = static_cast<int>(m_plate_data.size());

    if (!plate_stats.success)
        return;

    plate_stats.gcode_file = m_output_path;

    auto& modes = result.gcode_result.print_statistics.modes;
    auto& normal_mode = modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
    plate_stats.total_time = normal_mode.time;
    plate_stats.prepare_time = normal_mode.prepare_time;
    plate_stats.print_time = normal_mode.time - normal_mode.prepare_time;
    if (!std::isfinite(plate_stats.print_time))
    {
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << " print time is non-finite (" << normal_mode.time
                                   << " - " << normal_mode.prepare_time << "), falling back to 0";
        plate_stats.print_time = 0.0f;
    }

    plate_stats.total_filament_m = result.total_used_filament;
    plate_stats.total_filament_g = result.total_weight;
    plate_stats.total_cost = result.total_cost;
    plate_stats.support_used = result.support_used;
    plate_stats.toolpath_outside = result.gcode_result.toolpath_outside;
    plate_stats.long_retraction_when_cut = result.gcode_result.long_retraction_when_cut;

    // Prefer the post-trim diameters/densities captured at slice time (result.filament_*),
    // which match what the slicer applied. gcode_result mirrors the same merged config but
    // is kept as a fallback for paths that don't populate the snapshot (e.g. failed plates).
    //
    // NOTE: result.filament_diameters/densities are std::vector<double> (post-trim snapshot),
    // while GCodeProcessorResult::filament_diameters/densities are std::vector<float>. The two
    // cannot share one ?: branch. Downstream consumes both as `double`, so normalise to a
    // stable vector<double> owned by a named local (avoids a dangling reference to a temporary).
    auto pick_doubles = [](const std::vector<double>& snapshot,
                           const std::vector<float>& gcode) -> std::vector<double> {
        if (!snapshot.empty())
            return snapshot;                       // copy: vector<double> -> vector<double>
        return std::vector<double>(gcode.begin(), gcode.end());  // widen float -> double
    };
    const std::vector<double> fd    = pick_doubles(result.filament_diameters,
                                                   result.gcode_result.filament_diameters);
    const std::vector<double> fdens = pick_doubles(result.filament_densities,
                                                   result.gcode_result.filament_densities);

    for (const auto& [extruder_id, volume] : result.filament_volumes)
    {
        if (extruder_id < fd.size() && extruder_id < fdens.size())
        {
            double diameter = fd[extruder_id];
            double density = fdens[extruder_id];
            double cross_section = M_PI * 0.25 * diameter * diameter;
            double used_m = (volume / cross_section) * 0.001;
            double used_g = volume * density * 0.001;
            plate_stats.filament_used_m[extruder_id] = used_m;
            plate_stats.filament_used_g[extruder_id] = used_g;
        }
    }

    double total_support_m = 0.0, total_support_g = 0.0;
    double total_flush_m = 0.0, total_flush_g = 0.0;
    double total_wipe_tower_m = 0.0, total_wipe_tower_g = 0.0;

    auto& ps = result.gcode_result.print_statistics;
    auto accumulate = [&](const std::map<size_t, double>& volumes, double& m_acc, double& g_acc)
    {
        for (const auto& [extruder_id, volume] : volumes)
        {
            if (extruder_id < fd.size() && extruder_id < fdens.size())
            {
                double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                m_acc += (volume / cross_section) * 0.001;
                g_acc += volume * fdens[extruder_id] * 0.001;
            }
        }
    };
    accumulate(ps.support_volumes_per_extruder, total_support_m, total_support_g);
    accumulate(ps.wipe_tower_volumes_per_extruder, total_wipe_tower_m, total_wipe_tower_g);
    accumulate(ps.flush_per_filament, total_flush_m, total_flush_g);

    plate_stats.model_filament_m =
        plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
    plate_stats.model_filament_g =
        plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;

    // Prefer the post-trim nozzle diameters captured at slice time; fall back to the raw
    // m_config (which on a single-extruder plate still has N slots) for paths that never
    // populated the snapshot.
    if (!result.nozzle_diameters.empty())
    {
        plate_stats.nozzle_diameters = result.nozzle_diameters;
    }
    else if (m_config.has("nozzle_diameter"))
    {
        auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt)
            plate_stats.nozzle_diameters = nozzle_opt->values;
    }

    const ConfigOptionStrings* ftypes =
        m_config.has("filament_type") ? m_config.option<ConfigOptionStrings>("filament_type") : nullptr;
    const ConfigOptionStrings* fcolors =
        m_config.has("filament_colour") ? m_config.option<ConfigOptionStrings>("filament_colour") : nullptr;

    // Filament colours must match what was actually used at slice time, not the raw
    // m_config.  prepare_merged_config_for_plate may trim a multi-filament config down
    // to the single extruder a plate uses (remap slot keep_idx → 0), so m_config's
    // index 0 can point at a different colour than the one in the emitted G-code.
    // result.filament_colours holds the post-trim values captured in export_gcode from
    // print.config() (which mirrors the merged_config the slicer really applied) — read
    // from there first, fall back to m_config.
    const auto& result_colors = result.filament_colours;
    // Post-trim filament types captured at slice time — same rationale as the colours:
    // m_config's index 0 may point at a different slot after single-extruder remap, so read
    // the values that actually went into the G-code first, fall back to m_config.
    const auto& result_types = result.filament_types;

    for (const auto& [extruder_id, used_g] : plate_stats.filament_used_g)
    {
        SliceOutputStats::FilamentDetail detail;
        detail.id = extruder_id;
        detail.used_g = used_g;
        detail.used_m =
            plate_stats.filament_used_m.count(extruder_id) ? plate_stats.filament_used_m.at(extruder_id) : 0.0;

        const size_t ext_sz = static_cast<size_t>(extruder_id);
        if (ext_sz < result_types.size())
            detail.type = result_types[extruder_id];
        else if (ftypes && ext_sz < ftypes->values.size())
            detail.type = ftypes->values[extruder_id];
        else
            detail.type = "Unknown";

        if (ext_sz < result_colors.size())
            detail.color = result_colors[extruder_id];
        else if (fcolors && ext_sz < fcolors->values.size())
            detail.color = fcolors->values[extruder_id];
        else
            detail.color = "#000000";

        plate_stats.filament_details.push_back(detail);
    }

    for (const auto* pd : m_plate_data)
    {
        if (pd->plate_index == plate_id && !pd->thumbnail_file.empty())
        {
            plate_stats.model_thumbnail = "Metadata/plate_" + std::to_string(plate_id + 1) + ".png";
            break;
        }
    }
}

void SliceEngine::patch_orphan_plate_issues()
{
    // Add placeholder plates for plates with issues but not in plate_results.
    std::set<int> plates_with_results;
    for (const auto& p : m_stats.plates)
        plates_with_results.insert(p.plate_id);

    for (const auto& issue : m_stats.issues)
    {
        if (issue.plate_id < 0 || plates_with_results.find(issue.plate_id) != plates_with_results.end())
            continue;

        auto it = std::find_if(m_stats.plates.begin(), m_stats.plates.end(),
                               [&](const SliceOutputStats::PlateStats& p) { return p.plate_id == issue.plate_id; });
        if (it != m_stats.plates.end())
        {
            it->issues.push_back(issue);
            continue;
        }

        SliceOutputStats::PlateStats failed_plate;
        failed_plate.plate_id = issue.plate_id;
        failed_plate.success = false;
        failed_plate.plate_count = static_cast<int>(m_plate_data.size());
        failed_plate.issues.push_back(issue);
        plates_with_results.insert(issue.plate_id);
        m_stats.plates.push_back(failed_plate);
    }
}

void SliceEngine::finalise_statistics()
{
    auto by_severity = [](const Issue& a, const Issue& b)
    {
        return static_cast<int>(a.level) < static_cast<int>(b.level);
    };

    for (auto& p : m_stats.plates)
        std::stable_sort(p.issues.begin(), p.issues.end(), by_severity);
    std::stable_sort(m_stats.issues.begin(), m_stats.issues.end(), by_severity);

    std::sort(m_stats.plates.begin(), m_stats.plates.end(),
              [](const SliceOutputStats::PlateStats& a, const SliceOutputStats::PlateStats& b)
              { return a.plate_id < b.plate_id; });

    m_stats.success = !m_stats.plates.empty();
    for (const auto& p : m_stats.plates)
    {
        if (!p.success)
        {
            m_stats.success = false;
            break;
        }
    }
    if (m_stats.success)
    {
        for (const auto& issue : m_stats.issues)
        {
            if (issue.level == IssueLevel::error || issue.level == IssueLevel::serious_warning)
            {
                m_stats.success = false;
                m_any_error = true;
                set_error_type(EXIT_PREPROCESS_ERROR);
                break;
            }
        }
    }

    if (m_stats.success || !m_stats.error_message.empty())
        return;

    if (m_stats.plates.empty())
    {
        m_stats.error_message = "No plates completed successfully";
        return;
    }
    int failed_count = 0;
    for (const auto& p : m_stats.plates)
        if (!p.success)
            ++failed_count;
    if (failed_count == static_cast<int>(m_stats.plates.size()))
        m_stats.error_message = "All plates failed with errors";
    else if (failed_count > 0)
        m_stats.error_message = "Some plates failed with errors";
    else
        m_stats.error_message = "Global errors or serious warnings detected";
}
