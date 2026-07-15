#include "SliceEngine.hpp"
#include "GeometryCheck.hpp"
#include "Utils.hpp"

#include <cassert>
#include <cmath>
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

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

    // Config & preset validation
    validate_config();
    load_system_presets();
    if (!validate_presets())
    {
        build_statistics();
        return false;
    }

    if (!m_cfg.skip_preset_substitution)
    {
        // Apply the official Snapmaker U1 printer preset — clears custom G-code,
        // then wholesale-replaces printer config (printable_area, machine G-code,
        // nozzle_diameter, etc.) with official values.
        if (!apply_printer_official_preset())
        {
            build_statistics();
            return false;
        }

        // Filament official compliance check & substitution (always enforced).
        // Runs after printer preset so PresetRollback reads the corrected
        // nozzle_diameter from the official config, not the user's 3MF value.
        if (!apply_filament_official_preset())
        {
            build_statistics();
            return false;
        }

        // Substitute the process preset with the official Snapmaker U1 preset
        // so that process-level settings (skirt_loops, brim_type, etc.) from a
        // different printer profile in the 3MF don't leak through. User-explicit
        // overrides (different_settings_to_system) are preserved.
        // Non-blocking: substitution failure is a warning, not a fatal error.
        apply_process_official_preset();
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
    auto fail_load = [&](const std::string& code, const std::string& msg)
    {
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, code, msg));
        return false;
    };

    // --- Pre-load validation: format & size ---

    // Extension check (case-insensitive: .3mf, .3MF, .3Mf are all valid)
    std::string extension = boost::filesystem::path(m_cfg.input_file).extension().string();
    boost::to_lower(extension);
    if (extension != ".3mf")
    {
        return fail_load("FORMAT_REJECTED",
                         "Only .3mf files are supported. Please export your project from Snapmaker Orca Slicer.");
    }

    // File size check (configurable via --max-size, default 200MB, 0 = no limit)
    if (m_cfg.max_size_mb > 0)
    {
        boost::uintmax_t max_file_size = static_cast<boost::uintmax_t>(m_cfg.max_size_mb) * 1024ULL * 1024ULL;
        boost::system::error_code err_code;
        boost::uintmax_t file_size = boost::filesystem::file_size(m_cfg.input_file, err_code);
        if (!err_code && file_size > max_file_size)
        {
            return fail_load("FILE_SIZE_EXCEEDED",
                             "File size exceeds the limit (" + std::to_string(m_cfg.max_size_mb) +
                                 " MB). Please simplify the model or reduce face count and try again.");
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Loading 3MF file...";

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
        std::string what = e.what();
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << what;
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);

        // Detect gcode.3mf output files (no geometry, only pre-sliced G-code).
        // Model::read_from_file() throws "The supplied file couldn't be read
        // because it's empty" when the 3MF has valid XML metadata but zero
        // model objects — typical of a .gcode.3mf slicing result being
        // mistakenly re-submitted as input.
        bool is_empty = (what.find("empty") != std::string::npos);
        if (is_empty)
        {
            m_stats.error_message =
                "This 3MF file contains no 3D model objects. "
                "It appears to be a gcode.3mf slicing output file, not a project file. "
                "Please upload the original .3mf project file instead.";
            m_stats.issues.push_back(make_error(-1, "LOAD_3MF_ERROR", m_stats.error_message));
        }
        else
        {
            m_stats.error_message =
                "Failed to load 3MF file. The file may be corrupted or in an unsupported format.";
            m_stats.issues.push_back(make_error(-1, "LOAD_3MF_ERROR", m_stats.error_message));
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
        BOOST_LOG_TRIVIAL(error) << "No objects found in 3MF file";
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = "3MF file contains no sliceable model objects";
        m_stats.issues.push_back(make_error(-1, "MODEL_EMPTY", m_stats.error_message));
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Loaded " << m_model.objects.size() << " object(s)";

    // Detect and reject post-processing scripts in cloud mode (RCE prevention)
    if (m_config.has("post_process"))
    {
        auto* pp = m_config.option<ConfigOptionStrings>("post_process", true);
        if (pp && !pp->values.empty())
        {
            m_stats.issues.push_back(make_error(-1, "POST_PROCESS_REJECTED",
                                                "Custom post-processing scripts are not supported in cloud slicing."));
            m_config.set_key_value("post_process", new ConfigOptionStrings({}));
        }
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
// Stage 1.2: Config validation
// ============================================================================

void SliceEngine::validate_config()
{
    // A1: Validate config values (layer_height, nozzle_diameter, etc.).
    // Use under_cli=false to match desktop GUI behavior — invalid config
    // values produce warnings but do NOT block slicing.
    std::map<std::string, std::string> invalid = m_config.validate(false);
    for (const auto& [key, msg] : invalid)
        m_stats.issues.push_back(make_warning(-1, "CONFIG_INVALID_" + key, msg));

    // A2: Check config substitutions (unknown keys, forward-compat changes)
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
// Stage 1.3: Load system presets from vendor JSON files
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

// ============================================================================
// Stage 1.4: Validate presets against system profiles
// ============================================================================

bool SliceEngine::validate_presets()
{
    if (!m_presets_available || !m_preset_bundle)
    {
        BOOST_LOG_TRIVIAL(info) << "Preset validation skipped (system presets not available)";
        return true;
    }

    // B2: Load project embedded presets
    PresetBundle& preset_bundle = *m_preset_bundle;
    if (!m_project_presets.empty())
    {
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
            BOOST_LOG_TRIVIAL(warning) << "Failed to load project embedded presets: " << e.what();
        }
    }

    // B3: Validate presets against system profiles
    try
    {
        std::set<std::string> modified_gcodes;
        int validated = preset_bundle.validate_presets(m_cfg.input_file, m_config, modified_gcodes);

        switch (validated)
        {
        case VALIDATE_PRESETS_SUCCESS:
            BOOST_LOG_TRIVIAL(info) << "Preset validation passed";
            break;

        case VALIDATE_PRESETS_PRINTER_NOT_FOUND:
        {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom printer preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            BOOST_LOG_TRIVIAL(error) << msg;
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
            m_stats.error_message = msg;
            m_stats.issues.push_back(make_error(-1, "PRESET_PRINTER_NOT_FOUND", msg));
            return false;
        }

        case VALIDATE_PRESETS_FILAMENTS_NOT_FOUND:
        {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom filament preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_FILAMENT_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_MODIFIED_GCODES:
        {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Modified G-code keys found in presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_MODIFIED_GCODES", msg));
            break;
        }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Preset validation error: " << e.what();
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = std::string("Preset validation failed: ") + e.what();
        return false;
    }
}

// ============================================================================
// Stage 1.5a: Printer preset substitution (official Snapmaker U1)
// ============================================================================

bool SliceEngine::apply_printer_official_preset()
{
    // 1. Strip all custom G-code blocks — cloud slicing must not execute
    //    or embed user-supplied G-code for safety and consistency.
    //
    //    Coverage: all known machine-level (printer) and process-level G-code
    //    keys in the OrcaSlicer PrintConfig system. Filament-level keys
    //    (filament_start_gcode, filament_end_gcode) are handled separately by
    //    apply_filament_official_preset().
    //
    //    After clearing, official values are restored by the three-stage
    //    enforcement pipeline:
    //      overwrite_all_keys_from (printer) → official machine G-code
    //      apply_filament_official_preset     → official filament G-code
    //      apply_process_official_preset      → official process G-code
    //    Keys not defined in any official U1 preset remain blank — the U1
    //    runs Klipper firmware and does not use Marlin-era process-level
    //    G-code macros (start_gcode, end_gcode, etc.).
    {
        constexpr const char* gcode_keys[] = {
            // Printer (machine) level
            "machine_start_gcode",
            "machine_end_gcode",
            "before_layer_change_gcode",
            "layer_change_gcode",
            "change_filament_gcode",
            "machine_pause_gcode",
            // Process level
            "start_gcode",
            "end_gcode",
            "layer_gcode",
            "between_objects_gcode",
            "toolchange_gcode",
            "template_custom_gcode",
            "printing_by_object_gcode",
            "time_lapse_gcode",
            // Connectivity
            "print_host",
        };
        for (const char* key : gcode_keys)
        {
            if (m_config.has(key))
            {
                m_config.set_key_value(key, new ConfigOptionString(""));
                m_stats.issues.push_back(
                    make_tip(-1, "GCODE_CLEARED", std::string("Custom G-code '") + key + "' cleared for cloud safety"));
            }
        }
    }

    // 2. Replace the user's printer configuration wholesale with the official
    //    Snapmaker U1 preset matching the requested nozzle diameter. No user
    //    printer value (printable area, machine G-code, kinematics, …) is kept.
    //    The official machine G-code overwrites the cleared values above.
    //    The official preset config is sourced from the already-loaded
    //    PresetBundle, whose system presets carry a fully inherits-expanded
    //    config (fdm_U1 -> fdm_toolchanger merged in at load time).
    if (!m_presets_available || !m_preset_bundle)
    {
        std::string msg = "System presets not available; cannot apply official printer preset.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRINTER_PRESET_MISSING", msg));
        return false;
    }

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
    const Preset* official = m_preset_bundle->printers.find_preset(preset_name, false);
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

    // Wholesale overwrite: every key in the official config replaces the
    // user's value (machine G-code keys included).
    overwrite_all_keys_from(m_config, official->config);

    // Verify the official preset actually took effect: printable_area must be
    // a 4-point rectangle that differs from the library default, and
    // printable_height must differ from the default.
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
    auto near = [](double a, double b)
    {
        return std::abs(a - b) < PLATE_DIM_EPSILON;
    };
    bool is_default_area =
        (near(pa->values[0].x(), 0.0) && near(pa->values[0].y(), 0.0)) &&
        (near(pa->values[1].x(), DEFAULT_PLATE_WIDTH) && near(pa->values[1].y(), 0.0)) &&
        (near(pa->values[2].x(), DEFAULT_PLATE_WIDTH) && near(pa->values[2].y(), DEFAULT_PLATE_DEPTH)) &&
        (near(pa->values[3].x(), 0.0) && near(pa->values[3].y(), DEFAULT_PLATE_DEPTH));
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

    m_stats.issues.push_back(
        make_warning(-1, "PRINTER_SUBSTITUTED",
                     "Printer preset replaced with official preset \"" + preset_name + "\" for cloud safety"));
    return true;
}

// ============================================================================
// Stage 1.5b: Filament preset substitution (official compliance check)
// ============================================================================

bool SliceEngine::apply_filament_official_preset()
{
    if (!m_config.has("filament_settings_id"))
        return true;
    auto* filament_ids = m_config.option<ConfigOptionStrings>("filament_settings_id", true);
    if (!filament_ids || filament_ids->values.empty())
        return true;

    // Guaranteed by run(): apply_printer_official_preset() must succeed first
    assert(m_preset_bundle);

    // Lambda: check whether a system preset is "official"
    // Only Snapmaker vendor presets are supported in cloud deployment.
    auto is_official_preset = [](const Preset& p) -> bool
    {
        return p.vendor && p.vendor->name == PresetBundle::SM_BUNDLE;
    };

    // Look up a preset name: system presets first, then project embedded
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

    // All non-official failure branches funnel through here: first try
    // PresetRollback to fall back to a base category preset. On success,
    // emit a warning and treat as resolved; on failure, emit the original error.
    // After a successful rollback, filament_ids->values[i] is already updated
    // to the base category name.
    auto try_rollback = [&](int i, const std::string& name, const char* err_code, const std::string& err_msg) -> bool
    {
        if (PresetRollback::rollback(m_config, m_preset_bundle.get(), i))
        {
            const std::string& base_name = filament_ids->values[i];
            m_stats.issues.push_back(
                make_warning(-1, "FILAMENT_ROLLED_BACK",
                             "Filament \"" + name + "\" rolled back to base preset \"" + base_name + "\""));
            return true;
        }
        BOOST_LOG_TRIVIAL(error) << err_msg;
        m_stats.issues.push_back(make_error(-1, err_code, err_msg));
        return false;
    };

    // Resolve a single filament: official → true; unofficial → walk the
    // inheritance chain to find an official ancestor (full substitution);
    // on failure, rollback; if rollback also fails → emit error and return false.
    auto resolve_filament = [&](int i) -> bool
    {
        const std::string& name = filament_ids->values[i];

        // Case 1: Direct system preset match
        if (Preset* sys = find_in_system(name))
        {
            if (is_official_preset(*sys))
                return true; // OK
            return try_rollback(i, name, "FILAMENT_UNSUPPORTED_VENDOR",
                                "Filament \"" + name + "\" belongs to unsupported vendor");
        }

        // Case 2: Not a direct system match — walk the inheritance chain
        Preset* current = find_in_project(name);
        if (!current)
        {
            return try_rollback(i, name, "FILAMENT_UNKNOWN", "Filament \"" + name + "\" is not a recognized preset");
        }

        std::set<std::string> visited;
        while (current)
        {
            std::string inherits_name = current->inherits();
            if (inherits_name.empty())
            {
                return try_rollback(i, name, "FILAMENT_NO_OFFICIAL_ANCESTOR",
                                    "Filament \"" + name + "\" is not derived from any Snapmaker or Generic filament");
            }

            if (!visited.insert(inherits_name).second)
            {
                return try_rollback(i, name, "FILAMENT_CIRCULAR_INHERITS",
                                    "Circular inheritance detected in filament \"" + name + "\"");
            }

            // Try system presets first
            if (Preset* parent = find_in_system(inherits_name))
            {
                if (is_official_preset(*parent))
                {
                    substitute_filament_params(filament_ids, i, *parent, name);
                    return true;
                }
                std::string vendor_name = parent->vendor ? parent->vendor->name : "unknown";
                return try_rollback(i, name, "FILAMENT_UNSUPPORTED_VENDOR",
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
            return try_rollback(i, name, "FILAMENT_UNKNOWN_ANCESTOR",
                                "Filament \"" + name + "\" inherits from unknown preset \"" + inherits_name + "\"");
        }

        __builtin_unreachable(); // loop always exits via return
    };

    int num_filaments = static_cast<int>(filament_ids->values.size());
    bool any_error = false;
    for (int i = 0; i < num_filaments; ++i)
    {
        if (!resolve_filament(i))
            any_error = true;
    }
    if (any_error)
    {
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
    }
    return !any_error;
}

void SliceEngine::substitute_filament_params(ConfigOptionStrings* filament_ids, int ext_idx,
                                             const Preset& official_parent, const std::string& original_name)
{
    // Full overwrite: replace all per-extruder parameters in the target slot
    // with values from the official ancestor preset. No user values are
    // preserved. Shares the same helper with the PresetRollback fallback path.
    PresetRollback::overwriteExtruderFrom(m_config, official_parent, ext_idx, filament_ids);

    m_stats.issues.push_back(make_warning(-1, "FILAMENT_SUBSTITUTED",
                                          std::string("Filament \"") + original_name +
                                              "\" substituted with official preset \"" + official_parent.name + "\""));
}

// ============================================================================
// Stage 1.5c: Process preset substitution (official, preserves user overrides)
// ============================================================================

void SliceEngine::apply_process_official_preset()
{
    // Non-blocking: if system presets are unavailable, the existing process
    // config from the 3MF is used as a fallback. Printer config is already
    // correct at this point, so geometry is safe.
    if (!m_presets_available || !m_preset_bundle)
    {
        BOOST_LOG_TRIVIAL(warning)
            << "System presets not available; cannot apply official process preset.";
        return;
    }

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

    // Look up a process preset by name: system presets first, then project
    // embedded. Follows the same pattern as validate_filament_official().
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

    const Preset* official = nullptr;
    const std::string preset_name = dpp->value;
    // Case 1: Direct system preset match
    if (const Preset* sys = find_in_system(preset_name)) 
    {
        official = sys;
    } 
    else 
    {
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
                break;
            }

            if (const Preset* parent = find_in_system(inherits_name)) 
            {
                official = parent;
                break;
            }

            const Preset* project_parent = find_in_project(inherits_name);
            if (project_parent)
            {
                current = project_parent;
                continue;
            }
            break; // unknown ancestor
        }
    }

    if (official)
    {
        // Parse different_settings_to_system[0] — the set of process keys the
        // user explicitly changed from the original printer's system defaults.
        // These are preserved to honour the user's intent.
        std::set<std::string> user_overrides;
        {
            auto* diff_opt = m_config.option<ConfigOptionStrings>(
                "different_settings_to_system", false);
            if (diff_opt && !diff_opt->values.empty() && !diff_opt->values[0].empty())
            {
                std::istringstream ss(diff_opt->values[0]);
                std::string key;
                while (std::getline(ss, key, ';'))
                {
                    // Trim leading/trailing whitespace.
                    size_t start = key.find_first_not_of(" \t");
                    size_t end   = key.find_last_not_of(" \t");
                    if (start != std::string::npos && end != std::string::npos)
                        key = key.substr(start, end - start + 1);
                    if (!key.empty())
                        user_overrides.insert(key);
                }
            }
        }

        BOOST_LOG_TRIVIAL(info)
            << "Applying official process preset \"" << official->name
            << "\" (" << user_overrides.size() << " user overrides preserved)";

        // Apply every key from the official process preset, except keys the
        // user explicitly overrode and keys that don't exist in the current
        // config.
        overwrite_all_keys_from_except(m_config, official->config, user_overrides);

        m_stats.issues.push_back(make_warning(-1, "PROCESS_SUBSTITUTED",
            "Process preset replaced with official preset \"" + official->name
            + "\" for cloud consistency"));
    } 
    else 
    {
        BOOST_LOG_TRIVIAL(warning)
            << "No system process preset found for \"" << preset_name
            << "\" (not in system presets and no system ancestor in"
            << " inheritance chain); process settings not updated.";
    }

    // When brim_type is auto_brim, set brim_width to 0 so that the
    // fallback path (when the algorithm decides no brim is needed)
    // doesn't generate unwanted brim. The algorithm still sets its
    // own computed width when it determines brim IS needed.
    // Always executed, regardless of whether an official preset was found.
    {
        auto* bt = m_config.option<ConfigOptionEnum<BrimType>>("brim_type", false);
        if (bt && bt->value == btAutoBrim)
        {
            m_config.set_key_value("brim_width", new ConfigOptionFloat(0));
            BOOST_LOG_TRIVIAL(info)
                << "brim_type=auto_brim: brim_width set to 0 to match desktop behaviour";
        }
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
    for (auto& pd : m_plate_data)
    {
        if (pd->plate_thumbnail.pixels.empty())
            continue;

        Slic3r::png::ReadBuf buf{pd->plate_thumbnail.pixels.data(), pd->plate_thumbnail.pixels.size()};

        Slic3r::png::ImageColorscale img;
        if (!Slic3r::png::decode_colored_png(buf, img))
            continue;

        pd->plate_thumbnail.set(static_cast<unsigned int>(img.cols), static_cast<unsigned int>(img.rows));

        const size_t src_bpp = static_cast<size_t>(img.bytes_per_pixel);
        for (size_t y = 0; y < img.rows; ++y)
        {
            for (size_t x = 0; x < img.cols; ++x)
            {
                size_t src_idx = (y * img.cols + x) * src_bpp;
                size_t dst_idx = (y * img.cols + x) * 4;
                pd->plate_thumbnail.pixels[dst_idx + 0] = img.buf[src_idx + 0];
                pd->plate_thumbnail.pixels[dst_idx + 1] = img.buf[src_idx + 1];
                pd->plate_thumbnail.pixels[dst_idx + 2] = img.buf[src_idx + 2];
                pd->plate_thumbnail.pixels[dst_idx + 3] = (src_bpp >= 4) ? img.buf[src_idx + 3] : 255;
            }
        }
    }
}

// ============================================================================
// Stage 4: Process a single plate
// ============================================================================

void SliceEngine::process_plate(int plate_id)
{
    // Guaranteed by run(): apply_printer_official_preset() must succeed first
    assert(m_preset_bundle);

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

    // --- Slicing + Export ---
    // Check timeout before slicing
    if (m_has_timeout && std::chrono::steady_clock::now() > m_timeout_deadline)
    {
        BOOST_LOG_TRIVIAL(error) << "Slicing timed out for plate " << (plate_id + 1);
        m_stats.issues.push_back(make_error(plate_id, "SLICING_TIMEOUT",
                                            "Slicing timed out. The model may be too complex. If you believe this is "
                                            "an error, please submit an appeal for review."));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return;
    }

    // --- Create Print ---
    Print print;
    print.set_status_callback(
        [&print, this](const PrintBase::SlicingStatus& s)
        {
            default_status_callback(s, &print, &m_cfg.cancel_file);
        });
    print.is_BBL_printer() = m_preset_bundle->is_bbl_vendor();

    // --- Apply model ---
    if (!apply_model(plate_id, print, origin))
        return;

    // --- Assign arrange_order ---
    {
        int order = 1;
        for (ModelObject* obj : m_model.objects)
            for (ModelInstance* inst : obj->instances)
                inst->arrange_order = order++;
    }

    // --- Set global extruder params & speed table ---
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

    // --- Validation ---
    if (!run_validation(plate_id, print))
        return;

    // Slicing
    if (!run_slicing(plate_id, print))
    {
        BOOST_LOG_TRIVIAL(error) << "Slicing failed for plate " << (plate_id + 1);
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << (plate_id + 1);

    // Export G-code
    PlateSliceResult slice_result;
    if (!export_gcode(plate_id, print, slice_result))
    {
        if (is_wipe_tower_error(slice_result))
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
            BOOST_LOG_TRIVIAL(error) << "G-code export failed for plate " << (plate_id + 1);
        }
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return;
    }

    // Check for EmptyGcodeLayers before post-processing.
    // EmptyGcodeLayers means the plate has no valid layers; the G-code file
    // exists but is effectively empty. Skip post-processing and flag as failed.
    {
        bool has_empty_gcode_layers = false;
        for (const auto& iss : slice_result.issues)
        {
            if (iss.code == "PRINT_EMPTY_GCODE_LAYERS")
            {
                has_empty_gcode_layers = true;
                break;
            }
        }
        if (has_empty_gcode_layers)
        {
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
            return;
        }
    }

    run_postprocessing(plate_id, slice_result);

    // Free G-code visualization data that the cloud engine never uses.
    // GCodeProcessorResult::moves and lines_ends store every G-code move
    // vertex for the desktop GUI — hundreds of MB per plate.  Retaining
    // them across plates causes std::bad_alloc on complex multi-plate
    // projects (e.g. Mochi Makes plate 3 with 10 instances after plates
    // 1+2 already consumed 500+ MB for their moves vectors).
    slice_result.gcode_result.moves.clear();
    slice_result.gcode_result.moves.shrink_to_fit();
    slice_result.gcode_result.lines_ends.clear();
    slice_result.gcode_result.lines_ends.shrink_to_fit();

    m_plate_results[plate_id] = slice_result;
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
    {
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
        if (spiral_lift_active && build_volume.type() == BuildVolume_Type::Rectangle)
        {
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
                    if (min_dist < SPIRAL_LIFT_SAFETY_MARGIN)
                    {
                        if (warned_objects.insert(obj->name).second)
                        {
                            m_stats.issues.push_back(
                                make_serious_warning(plate_id, "SPIRAL_LIFT_NEAR_BOUNDARY",
                                             "Model too close to bed boundary. "
                                             "Disable spiral lifting or keep at least 3.5mm gap to avoid collision.",
                                             obj->name));
                            m_any_postprocess_warning = true;
                        }
                    }
                }
            }
        }
    }

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

bool SliceEngine::apply_model(int plate_id, Print& print, const Vec3d& origin)
{
    // plate_index from m_plate_data is already 0-based (import does -1 conversion)
    print.set_plate_index(plate_id);

    // Use the grid-layout-computed origin so object positions in gcode match
    // the desktop output (PartPlate::update_plate_layout_arrange).
    print.set_plate_origin(origin);

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
    {
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
        // (used by non-Bambu printers for single-nozzle multi-filament).
        if (used_extruders.size() <= 1 && num_filaments > 1)
        {
            auto* semm = m_config.option<ConfigOptionBool>("single_extruder_multi_material");
            if (semm && semm->value)
            {
                // Model genuinely uses multiple filaments through one extruder.
                // Insert sentinel values to prevent trimming.
                used_extruders.insert(1);
                used_extruders.insert(2);
            }
        }

        if (used_extruders.size() <= 1 && num_filaments > 1)
        {
            BOOST_LOG_TRIVIAL(info) << "Trimming filament config from " << num_filaments
                                    << " to 1 to match single-extruder model";
            merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

            // Truncate all filament-related array options to 1 entry.
            // This prevents Print::has_wipe_tower() from returning true due to
            // filament_diameter.size() > 1, which is the root cause of the
            // "append_tcr was asked to do a toolchange it didn't expect" error.
            // flush_volumes_matrix (N*N flat vector) and wiping_volumes_extruders
            // must also be trimmed to keep the config internally consistent:
            // a 5*5 matrix with only 1 extruder would mismatch sqrt(size) later.
            constexpr const char* trim_keys[] = {
                "filament_diameter",    "filament_density",         "filament_cost",        "filament_colour",
                "filament_type",        "filament_is_support",      "filament_settings_id", "nozzle_diameter",
                "flush_volumes_matrix", "wiping_volumes_extruders",
            };
            for (const char* key : trim_keys)
            {
                auto* opt = merged_config.option(key, true);
                if (!opt)
                    continue;
                if (auto* fs = dynamic_cast<ConfigOptionFloats*>(opt))
                {
                    if (!fs->values.empty())
                    {
                        fs->values.resize(1);
                        merged_config.set_key_value(key, new ConfigOptionFloats(fs->values));
                    }
                }
                else if (auto* ss = dynamic_cast<ConfigOptionStrings*>(opt))
                {
                    if (!ss->values.empty())
                    {
                        ss->values.resize(1);
                        merged_config.set_key_value(key, new ConfigOptionStrings(ss->values));
                    }
                }
                else if (auto* bs = dynamic_cast<ConfigOptionBools*>(opt))
                {
                    if (!bs->values.empty())
                    {
                        bs->values.resize(1);
                        merged_config.set_key_value(key, new ConfigOptionBools(bs->values));
                    }
                }
            }
        }
        else if (used_extruders.size() <= 1)
        {
            BOOST_LOG_TRIVIAL(info) << "Disabling prime tower (single extruder model)";
            merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
        }
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
    print.apply(m_model, merged_config);

    if (print.num_object_instances() == 0)
    {
        m_stats.issues.push_back(
            make_warning(plate_id, "NO_PRINTABLE_OBJECTS", "No printable objects on this plate after apply"));
        return false;
    }

    return true;
}

bool SliceEngine::run_validation(int plate_id, Print& print)
{
    StringObjectException warning;
    StringObjectException err = print.validate(&warning, nullptr, nullptr);

    // --- #6: Refine bed mismatch error/warning messages (desktop parity) ---
    // Replace extruder number with user-friendly filament name.
    auto refine_bed_mismatch_message = [&](StringObjectException& ex) {
        if (ex.type == STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE
            && m_preset_bundle
            && ex.params.size() >= 3)
        {
            try {
                int extruder_idx = std::stoi(ex.params[2]) - 1;
                if (extruder_idx >= 0
                    && extruder_idx < static_cast<int>(m_preset_bundle->filament_presets.size()))
                {
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
            } catch (...) { /* keep original message */ }
        }
    };
    refine_bed_mismatch_message(warning);
    refine_bed_mismatch_message(err);

    if (!warning.string.empty())
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
        default:
            wcode = "PRINT_VALIDATE_WARNING";
            break;
        }
        m_stats.issues.push_back(make_warning(plate_id, wcode, warning.string + opt_hint, obj_name));
    }

    if (!err.string.empty())
    {
        auto [obj_name, opt_hint] = format_exception_context(err);
        // STRING_EXCEPT_NOT_DEFINED (type 0) is used by the library for
        // generic checks (e.g. exceeds-build-volume-height) that the
        // desktop GUI treats as non-fatal warnings.  Match that behavior.
        if (err.type == STRING_EXCEPT_NOT_DEFINED)
        {
            BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;
            m_stats.issues.push_back(make_warning(plate_id, "PRINT_VALIDATE_WARNING",
                                                  err.string + opt_hint, obj_name));
        }
        else
        {
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
            default:
                ecode = "PRINT_VALIDATE_ERROR";
                break;
            }
            m_stats.issues.push_back(make_error(plate_id, ecode, err.string + opt_hint, obj_name));
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
            return false;
        }
    }

    // --- #1: Snapmaker U1 + Print By Object caution (desktop parity) ---
    if (print.config().print_sequence.value == PrintSequence::ByObject) {
        m_stats.issues.push_back(make_warning(
            plate_id, "PRINT_BY_OBJECT_CAUTION",
            "Printing by object with caution. This function may cause the print head "
            "to collide with printed parts during switching."));
    }

    // --- #2: Filament/nozzle/bed compatibility checks (desktop parity) ---
    {
        NozzleFilamentRuleMismatch nozzle_mismatch;
        bool is_gesp = false, is_pei_not_pla = false, is_pei_tpu = false;

        print.filament_rule_mismatch_flags(nozzle_mismatch, is_gesp,
                                           is_pei_not_pla, is_pei_tpu,
                                           m_preset_bundle.get());

        // A: Nozzle mismatch
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

        // D: PEI + TPU (higher priority than C, per desktop behavior)
        if (is_pei_tpu) {
            m_stats.issues.push_back(make_warning(
                plate_id, "FILAMENT_BED_PEI_TPU_STICKING",
                "Filament may stick too strongly to the smooth PEI plate. "
                "Apply glue to protect the plate and ease part removal."));
        }

        // C: PEI + non-PLA (suppressed if D also triggered)
        if (is_pei_not_pla && !is_pei_tpu) {
            m_stats.issues.push_back(make_warning(
                plate_id, "FILAMENT_BED_PEI_ADHESION",
                "Filament may not adhere well to the smooth PEI plate on "
                "the first layer. Apply glue before printing."));
        }

        // B: GESP
        if (is_gesp) {
            m_stats.issues.push_back(make_warning(
                plate_id, "FILAMENT_BED_GESP_ADHESION",
                "Low adhesion to the graphic effect plate may cause failure. "
                "Use a different filament instead."));
        }
    }

    // --- #3: High/low temperature filament mixing check (desktop parity) ---
    // Uses filament_is_high_temperature from config, same data source as the
    // desktop GUI's check_filament_temp_mixing and chamber_cooling_mode.
    {
        bool has_high = false, has_low = false;
        for (unsigned int extruder : print.extruders()) {
            if (print.config().filament_is_high_temperature.get_at(extruder))
                has_high = true;
            else
                has_low = true;
        }
        if (has_high && has_low) {
            std::string msg =
                "Cannot print multiple filaments which have large difference of "
                "temperature together. Otherwise, the extruder and nozzle may be "
                "blocked or damaged during printing.";
            BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": " << msg;
            m_stats.issues.push_back(make_error(
                plate_id, "FILAMENT_TEMP_MIXING", msg));
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
            return false;
        }
    }

    return true;
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
        BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << plate_id << ": " << e.what();
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

void SliceEngine::run_postprocessing(int plate_id, PlateSliceResult& result)
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

// ============================================================================
// Stage 5: Package output as gcode.3mf
// ============================================================================

void SliceEngine::package_output()
{
    BOOST_LOG_TRIVIAL(info) << "Creating gcode.3mf package...";

    std::string printer_model_id;
    std::string nozzle_diameters_str;

    if (m_config.has("printer_model"))
        printer_model_id = m_config.opt_string("printer_model");

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
            nozzle_diameters_str = ss.str();
        }
    }

    const ConfigOptionStrings* filament_types = nullptr;
    const ConfigOptionStrings* filament_colors = nullptr;
    const ConfigOptionStrings* filament_ids = nullptr;

    if (m_config.has("filament_type"))
        filament_types = m_config.option<ConfigOptionStrings>("filament_type");
    if (m_config.has("filament_colour"))
        filament_colors = m_config.option<ConfigOptionStrings>("filament_colour");
    if (m_config.has("filament_ids"))
        filament_ids = m_config.option<ConfigOptionStrings>("filament_ids");

    for (auto& pd : m_plate_data)
    {
        auto it = m_plate_results.find(pd->plate_index);
        if (it == m_plate_results.end()) continue;

        PlateSliceResult& result = it->second;
        pd->gcode_file = result.gcode_path;
        pd->is_sliced_valid = true;
        pd->printer_model_id = printer_model_id;
        pd->nozzle_diameters = nozzle_diameters_str;

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
            if (filament_types && idx < filament_types->values.size())
                info.type = filament_types->values[idx];
            if (filament_colors && idx < filament_colors->values.size())
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

    // Thumbnail data: one default-constructed (reset) ThumbnailData per plate,
    // every pointer non-NULL but is_valid() == false.  Prevents NULL deref in
    // headless environments (no GPU / Mesa / EGL) while the fallback path in
    // _BBS_3MF_Exporter still picks up PNG files from plate_data on disk.
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

    params.project = nullptr;
    params.profile = nullptr;

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

        if (plate_stats.success)
        {
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

            // Calculate model filament
            double total_support_m = 0.0, total_support_g = 0.0;
            double total_flush_m = 0.0, total_flush_g = 0.0;
            double total_wipe_tower_m = 0.0, total_wipe_tower_g = 0.0;

            const auto& fd = result.gcode_result.filament_diameters;
            const auto& fdens = result.gcode_result.filament_densities;

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

            auto& ps = result.gcode_result.print_statistics;
            for (const auto& [extruder_id, volume] : ps.support_volumes_per_extruder)
            {
                if (extruder_id < fd.size() && extruder_id < fdens.size())
                {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_support_m += (volume / cross_section) * 0.001;
                    total_support_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.wipe_tower_volumes_per_extruder)
            {
                if (extruder_id < fd.size() && extruder_id < fdens.size())
                {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_wipe_tower_m += (volume / cross_section) * 0.001;
                    total_wipe_tower_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.flush_per_filament)
            {
                if (extruder_id < fd.size() && extruder_id < fdens.size())
                {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_flush_m += (volume / cross_section) * 0.001;
                    total_flush_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            plate_stats.model_filament_m =
                plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
            plate_stats.model_filament_g =
                plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;

            if (m_config.has("nozzle_diameter"))
            {
                auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
                if (nozzle_opt)
                    plate_stats.nozzle_diameters = nozzle_opt->values;
            }

            plate_stats.plate_count = static_cast<int>(m_plate_data.size());

            const ConfigOptionStrings* ftypes =
                m_config.has("filament_type") ? m_config.option<ConfigOptionStrings>("filament_type") : nullptr;
            const ConfigOptionStrings* fcolors =
                m_config.has("filament_colour") ? m_config.option<ConfigOptionStrings>("filament_colour") : nullptr;

            for (const auto& [extruder_id, used_g] : plate_stats.filament_used_g)
            {
                SliceOutputStats::FilamentDetail detail;
                detail.id = extruder_id;
                detail.used_g = used_g;
                detail.used_m =
                    plate_stats.filament_used_m.count(extruder_id) ? plate_stats.filament_used_m.at(extruder_id) : 0.0;

                const size_t ext_sz = static_cast<size_t>(extruder_id);
                if (ftypes && ext_sz < ftypes->values.size())
                    detail.type = ftypes->values[extruder_id];
                else
                    detail.type = "Unknown";

                if (fcolors && ext_sz < fcolors->values.size())
                    detail.color = fcolors->values[extruder_id];
                else
                    detail.color = "#000000";

                plate_stats.filament_details.push_back(detail);
            }

            // Look up the thumbnail path from plate data
            for (const auto* pd : m_plate_data)
            {
                if (pd->plate_index == plate_id && !pd->thumbnail_file.empty())
                {
                    plate_stats.model_thumbnail = "Metadata/plate_" + std::to_string(plate_id + 1) + ".png";
                    break;
                }
            }
        }
        else
        {
            plate_stats.plate_count = static_cast<int>(m_plate_data.size());
        }

        m_stats.plates.push_back(plate_stats);
    }

    // Add placeholder plates for plates with issues but not in plate_results
    std::set<int> plates_with_results;
    for (const auto& p : m_stats.plates)
        plates_with_results.insert(p.plate_id);

    for (const auto& issue : m_stats.issues)
    {
        if (issue.plate_id >= 0 && plates_with_results.find(issue.plate_id) == plates_with_results.end())
        {
            bool found = false;
            for (const auto& p : m_stats.plates)
            {
                if (p.plate_id == issue.plate_id)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                SliceOutputStats::PlateStats failed_plate;
                failed_plate.plate_id = issue.plate_id;
                failed_plate.success = false;
                failed_plate.plate_count = static_cast<int>(m_plate_data.size());
                failed_plate.issues.push_back(issue);
                plates_with_results.insert(issue.plate_id);
                m_stats.plates.push_back(failed_plate);
            }
            else
            {
                for (auto& p : m_stats.plates)
                {
                    if (p.plate_id == issue.plate_id)
                    {
                        p.issues.push_back(issue);
                        break;
                    }
                }
            }
        }
    }

    // Sort issues by severity within each plate (stable: preserves detection order within same level)
    for (auto& p : m_stats.plates)
    {
        std::stable_sort(p.issues.begin(), p.issues.end(),
                         [](const Issue& a, const Issue& b)
                         {
                             return static_cast<int>(a.level) < static_cast<int>(b.level);
                         });
    }

    // Sort global issues by severity
    std::stable_sort(m_stats.issues.begin(), m_stats.issues.end(),
                     [](const Issue& a, const Issue& b)
                     {
                         return static_cast<int>(a.level) < static_cast<int>(b.level);
                     });

    // Sort plates by plate_id
    std::sort(m_stats.plates.begin(), m_stats.plates.end(),
              [](const SliceOutputStats::PlateStats& a, const SliceOutputStats::PlateStats& b)
              {
                  return a.plate_id < b.plate_id;
              });

    // Determine global success
    m_stats.success = !m_stats.plates.empty();
    for (const auto& p : m_stats.plates)
    {
        if (!p.success)
        {
            m_stats.success = false;
            break;
        }
    }
    // Also check global-level issues for errors/serious warnings that bypassed per-plate checks
    if (m_stats.success)
    {
        for (const auto& issue : m_stats.issues)
        {
            if (issue.level == IssueLevel::error || issue.level == IssueLevel::serious_warning)
            {
                m_stats.success = false;
                break;
            }
        }
    }
    if (!m_stats.success && m_stats.error_message.empty())
    {
        if (m_stats.plates.empty())
            m_stats.error_message = "No plates completed successfully";
        else
        {
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
    }
}
