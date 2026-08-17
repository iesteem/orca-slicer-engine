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
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/CustomGCode.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/FilamentHotBedNozzleRules.hpp"

#include "PresetRollback.hpp"
#include "PlateGrid.hpp"
#include "SlicingDeadline.hpp"
#include "BuildVolumeClassify.hpp"
#include "ValidateClassify.hpp"

using namespace Slic3r;

namespace
{

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

// ----------------------------------------------------------------------------
// File-local free-function forward declarations.
//
// These helpers do not touch any SliceEngine member state (pure input→output),
// so they are kept as file-local `static` functions rather than members. They
// are defined further down but declared here because call sites in member
// functions appear before their definitions.
// ----------------------------------------------------------------------------
static void decode_one_plate_thumbnail(Slic3r::PlateData& pd);
static bool  all_gcode_layers_valid(const PlateSliceResult& slice_result);
static void  remove_unusable_gcode(int plate_id, PlateSliceResult& slice_result);

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

    // Runs even when substitution was skipped: both fixes guard against
    // libslic3r SEGVs and are independent of the substitution policy.
    normalize_loaded_config();

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
            bake_instance_z_into_mesh();

            // Assign a global, monotonic arrange_order to every instance once per
            // task. This is model-wide (plate-agnostic), so it belongs here rather
            // than inside the per-plate process_plate loop, where it was previously
            // re-written with identical values N times. Note libslic3r re-derives
            // the final ordering during Print::validate() / GCode export
            // (sort_object_instances_by_model_order); this just seeds a non-zero
            // value on m_model before any Print copies it.
            assign_arrange_order();

            // Populate Model::extruderParamsMap once per task. Config-only (reads
            // m_config, which is final after apply_filament_official_preset in
            // validate_input above), writes a static Model member, no Print or
            // plate dependency -- so it belongs here, not in the per-plate loop
            // where it was previously re-run with identical results N times.
            setup_extruder_params();

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

bool SliceEngine::run_preset_substitution_only()
{
    // Config-only prefix of run(): load the project, validate the printer
    // model, load system/project presets, and apply official substitution.
    // No geometry checks, slicing, or export. The call sequence mirrors the
    // prefix of run() (SliceEngine.cpp:146-183) and MUST stay in sync with it.
    if (!load_3mf())
    {
        build_statistics();
        return false;
    }

    if (!validate_printer_model())
    {
        build_statistics();
        return false;
    }

    // Collect config warnings (never blocks the pipeline).
    collect_config_warnings();

    // Load vendor + project presets so apply_*_official_preset can resolve.
    load_system_presets();
    load_project_presets();

    if (!m_cfg.skip_preset_substitution)
    {
        if (!apply_preset_substitution())
        {
            build_statistics();
            return false;
        }
    }

    normalize_loaded_config();

    // m_config now holds the substitution result; run()'s later stages do not
    // modify the preset keys, so callers can read it via config().
    build_statistics();
    return !m_any_error;
}

bool SliceEngine::run_geometry_preprocess_only()
{
    // Config + geometry-preprocessing prefix of run(): everything in run() up to
    // and including setup_extruder_params (SliceEngine.cpp:146-226), minus the
    // per-plate slicing loop, package_output, and export. The geometry stages
    // mutate m_model in place (Z baked into mesh, arrange_order stamped); after
    // return it is readable via model(). No slicing, no export.
    //
    // The call sequence mirrors the prefix of run() and MUST stay in sync.

    // --- Config half (same prefix as run_preset_substitution_only) ---
    if (!load_3mf())
    {
        build_statistics();
        return false;
    }

    if (!validate_printer_model())
    {
        build_statistics();
        return false;
    }

    collect_config_warnings();
    load_system_presets();
    load_project_presets();

    if (!m_cfg.skip_preset_substitution)
    {
        if (!apply_preset_substitution())
        {
            build_statistics();
            return false;
        }
    }

    normalize_loaded_config();

    // --- Geometry-half preprocessing (mirrors run():185-226) ---
    if (!validate_input())
    {
        build_statistics();
        return false;
    }

    try
    {
        // Geometry defect detection (once for the whole model, before per-plate).
        {
            auto geom_issues = run_geometry_checks(m_model);
            for (auto& issue : geom_issues)
            {
                m_stats.issues.push_back(std::move(issue));
            }
        }

        // Bake instance Z into mesh + seat objects on bed (desktop parity).
        bake_instance_z_into_mesh();

        // Stamp a global, monotonic arrange_order on every instance.
        assign_arrange_order();

        // Populate Model::extruderParamsMap from the final config.
        setup_extruder_params();
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Unhandled exception in geometry preprocessing: " << e.what();
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        m_stats.issues.push_back(make_error(
            -1, "INTERNAL_ERROR",
            "An unexpected internal error occurred during geometry preprocessing. Please try again or contact support."));
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "Unhandled non-standard exception in geometry preprocessing";
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        m_stats.issues.push_back(
            make_error(-1, "INTERNAL_FATAL", "Unhandled unknown exception in geometry preprocessing"));
    }

    build_statistics();
    return !m_any_error;
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

// Normalise flush_volumes_matrix / flush_volumes_vector to match the actual
// filament slot count. 3MF files authored by editing the AMS colour palette
// sometimes ship a flush matrix whose dimension no longer matches the filament
// count (e.g. a 3x3 matrix with 4 filament slots). The desktop GUI repairs this
// in PresetBundle::update_multi_material_filament_presets; the headless engine
// has no GUI, so it must do the same here. Without it, Print::_make_wipe_tower
// (libslic3r) hits its `filament_diameter.size() > sqrt(matrix.size())` guard,
// skips wipe-tower/tool-ordering construction, and the subsequent skirt&brim
// step dereferences an empty ToolOrdering -> SIGSEGV. Ported from
// PresetBundle.cpp:3650-3675 (upstream OrcaSlicer).
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

    // Resize flush_volumes_vector to 2*num_filaments (default 140. per upstream).
    auto* v_opt = config.option<ConfigOptionFloats>("flush_volumes_vector");
    std::vector<double> filaments = v_opt ? v_opt->values : std::vector<double>{};
    while (filaments.size() < 2 * num_filaments) {
        filaments.push_back(filaments.size() > 1 ? filaments[0] : 140.);
        filaments.push_back(filaments.size() > 1 ? filaments[1] : 140.);
    }
    while (filaments.size() > 2 * num_filaments) {
        filaments.pop_back();
        filaments.pop_back();
    }

    // Rebuild the matrix to num_filaments x num_filaments: copy old pairs that
    // still fit, synthesise the rest (diagonal=0, off-diagonal=load+unload).
    std::vector<double> new_matrix;
    new_matrix.reserve(num_filaments * num_filaments);
    for (size_t i = 0; i < num_filaments; ++i)
        for (size_t j = 0; j < num_filaments; ++j) {
            if (i < old_n && j < old_n)
                new_matrix.push_back(old_matrix[i * old_n + j]);
            else
                new_matrix.push_back(i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
        }

    m_opt->values = std::move(new_matrix);
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

void SliceEngine::bake_instance_z_into_mesh()
{
    // Relocate each object's Z so it survives libslic3r's Print::apply, which
    // zeroes the Z component of every instance transform ("Z offset is
    // discarded to ensure first layer starts at Z=0", PrintApply.cpp:155).
    // An object stored with instance.z != 0 would have that Z silently
    // dropped, clipping whatever the author placed above/below the bed.
    //
    // Strategy: move all Z information out of instance space into mesh-vertex
    // space, where Print::apply cannot touch it. Two phases per object:
    //
    //   Step 1 -- bake the existing instance.z offset into the mesh, then
    //             zero every instance.z. After this, instance.z is 0 for all
    //             instances of this object, and the mesh has been translated
    //             by the equivalent local-space Z.
    //
    //   Step 2 -- apply the desktop "intentional sinking" rule
    //             (allow_negative_z=true semantics from ModelObject::
    //              ensure_on_bed) to decide whether the object also needs
    //             raising, and if so bake that raise into the mesh too.
    //
    // Single-part sinking rule (mirrors Model.cpp:1829-1835):
    //   - min_z >= SINKING_Z_THRESHOLD (-0.001)  -> already on bed, no raise
    //   - max_z < 0                              -> fully buried, raise to bed
    //   - min_z < 0 AND max_z >= 0              -> straddling: author
    //                                              intentionally sank it,
    //                                              leave in place
    // Multi-part rule (mirrors Model.cpp:1836-1840): raise only when the
    // whole object's max_z < SINKING_MIN_Z_THRESHOLD (0.05). Multi-part
    // objects do not get the "intentional straddling" carve-out -- only
    // their max_z matters.
    //
    // Note (assumption): baking Z into the mesh assumes every instance of
    // this ModelObject shares the same rotation. libslic3r's ModelObject
    // uses one mesh shared across instances, and our world->local Z
    // conversion uses instances.front()'s rotation matrix. Per-instance
    // rotation with non-zero Z is not supported by this path. Current
    // callers (3MF import + arrange) never produce that combination; if
    // they ever do, the bake will be correct only for the first instance.
    //
    // Side effect on the world position of the object: ideally zero. We
    // compensate every instance-space mutation with an equal and opposite
    // mesh-space mutation, so the final world-space Z equals what
    // ensure_on_bed(true) would have produced -- but lives entirely in
    // mesh-vertex space, safe from Print::apply.
    for (ModelObject* obj : m_model.objects)
    {
        if (obj->instances.empty())
            continue;
        // Force fresh bounding-box computation. The cache may be stale from
        // 3MF import before instance transforms were finalized.
        obj->invalidate_bounding_box();

        // Local helper: convert a world-space (0,0,dz) shift to local mesh
        // space using the front instance's rotation (no offset component).
        // translate() writes mesh vertices in local space; if the instance
        // is rotated, the world-Z axis is not aligned with local-Z, so the
        // shift must be un-rotated first.
        auto world_z_to_local = [obj](double dz) {
            auto rot_no_off = obj->instances.front()->get_transformation().get_matrix_no_offset();
            Vec3d world_shift(0, 0, dz);
            return rot_no_off.inverse() * world_shift;
        };

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
            Vec3d local_shift = world_z_to_local(inst_z);
            obj->translate(local_shift.x(), local_shift.y(), local_shift.z());
            // Zero all instance Z offsets.
            for (ModelInstance* inst : obj->instances)
            {
                Vec3d off = inst->get_offset();
                inst->set_offset(Vec3d(off.x(), off.y(), 0.0));
            }
        }

        // --- Step 2: apply the desktop sinking rule, baking any raise into the mesh ---
        //
        // We do NOT call ModelObject::ensure_on_bed here. ensure_on_bed's only
        // side effect is to mutate instance offset (translate_instances ->
        // set_offset, Model.cpp:1845-1862); it never touches mesh vertices.
        // Using it would force the same "call then undo the side effect"
        // pattern the previous implementation relied on -- readable only if
        // the reader remembers that detail, and brittle across libslic3r
        // versions if that contract ever changes.
        //
        // Instead we replicate the allow_negative_z=true decision (Model.cpp:
        // 1829-1841) locally to compute z_shift, then bake z_shift into the
        // mesh ourselves. Same final world-space Z as ensure_on_bed(true),
        // but the raise lives in mesh-vertex space where Print::apply cannot
        // discard it.
        constexpr double kSinkingZThreshold     = -0.001;  // Model.hpp: SINKING_Z_THRESHOLD
        constexpr double kSinkingMinZThreshold   = 0.05;   // Model.hpp: SINKING_MIN_Z_THRESHOLD
        constexpr double kEpsilon                 = 1e-4;

        {
            obj->invalidate_bounding_box();
            const double min_z_before = obj->min_z();
            const double max_z_before = obj->max_z();  // for diagnostics; geometry never mutates here
            const size_t  parts       = obj->parts_count();

            // Replicate ensure_on_bed(allow_negative_z=true):
            //   single-part: raise only if (min_z >= -0.001) or (max_z < 0).
            //                Straddling (min_z < 0 AND max_z >= 0) is left alone.
            //   multi-part:  raise only if max_z < 0.05.
            // Straddling single-part objects are the author's intentional
            // sinking and are preserved exactly; the multi-part rule has no
            // such carve-out, only an absolute max_z cutoff.
            double z_shift = 0.0;
            if (parts == 1)
            {
                if (min_z_before >= kSinkingZThreshold || max_z_before < 0.0)
                    z_shift = -min_z_before;
            }
            else
            {
                if (max_z_before < kSinkingMinZThreshold)
                    z_shift = kSinkingMinZThreshold - max_z_before;
            }

            if (std::abs(z_shift) > kEpsilon)
            {
                Vec3d local_shift = world_z_to_local(z_shift);
                obj->translate(local_shift.x(), local_shift.y(), local_shift.z());
                // No instance offset change: the raise is entirely in the mesh.
            }

            obj->invalidate_bounding_box();
            if (std::abs(z_shift) > 1e-6 || std::abs(min_z_before) > 1e-6)
            {
                BOOST_LOG_TRIVIAL(info) << "bake_instance_z_into_mesh: object \"" << obj->name
                                        << "\" inst_z=" << inst_z
                                        << " parts=" << parts
                                        << " z_shift=" << z_shift;
            }

            // Diagnostic warning when the object's stored Z sat below the bed.
            // Two distinct cases, kept separate because their semantics are
            // different and the previous combined text conflated them:
            //
            //   (a) z_shift != 0  -> The sinking rule fired. The object was
            //      either fully buried below the bed or its bottom was
            //      essentially on the bed and got snapped up. Either way we
            //      raised it; surface that so the silent correction is
            //      visible.
            //
            //   (b) z_shift == 0 AND min_z_before < 0  -> Only reachable for
            //      single-part objects: straddling (min_z<0 AND max_z>=0).
            //      This is a genuine author-intended sinking (e.g. a base
            //      flattened against the bed) and is preserved as stored.
            //      Multi-part objects with max_z >= 0.05 never raise, but
            //      they also don't reach here as "intentional" -- they were
            //      simply above the threshold, not sank -- so the
            //      "intentional" wording is only used for the single-part
            //      straddle case.
            //
            // We include the original instance.z in the message so a user
            // debugging can tell apart "stored with instance.z=0 and a
            // negative mesh" from "stored with instance.z=N and a mesh that
            // combined to a negative world Z" -- both produce the same
            // post-Step-1 mesh state, but the upstream project state differs.
            if (min_z_before < -kEpsilon)
            {
                if (std::abs(z_shift) > kEpsilon)
                {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf),
                                  "Object \"%s\" was stored below the print bed "
                                  "(min Z=%.2fmm, original instance Z=%.2fmm, parts=%zu); "
                                  "auto-raised by %.2fmm to sit on the bed.",
                                  obj->name.c_str(), min_z_before, inst_z, parts, z_shift);
                    m_stats.issues.push_back(make_warning(
                        -1, "OBJECT_BELOW_BED_ADJUSTED", buf, obj->name,
                        "If the sinking was intentional, reposition the object's Z in your slicer before exporting."));
                }
                else
                {
                    // Reached when no raise was applied but min_z_before < 0.
                    // Two sub-cases land here:
                    //   - single-part straddling (min_z<0 AND max_z>=0):
                    //     the author intentionally sank the base -- preserved
                    //     as stored, the "intentionally straddles" wording is
                    //     accurate.
                    //   - multi-part with max_z >= 0.05 and min_z < 0:
                    //     the multi-part rule did not fire because only max_z
                    //     matters; the object is partially below the bed but
                    //     was not deliberately sank in the single-part sense.
                    //     The wording below is slightly off for this sub-case
                    //     (it's not "intentional"), but we keep one issue code
                    //     to match the previous behavior. The "parts=N" field
                    //     lets users tell the two apart.
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                                  "Object \"%s\" intentionally straddles the print bed "
                                  "(Z %.2f to %.2fmm, original instance Z=%.2fmm, parts=%zu); "
                                  "preserved as-is, the below-bed portion will not be printed.",
                                  obj->name.c_str(), min_z_before, max_z_before, inst_z, parts);
                    m_stats.issues.push_back(make_warning(
                        -1, "OBJECT_INTENTIONALLY_BELOW_BED", buf, obj->name,
                        "Reposition the object in your slicer if the below-bed portion was meant to be printed."));
                }
            }
        }
    }
}

// Global model/config preparation, run once from run() before the per-plate loop
// (after bake_instance_z_into_mesh, before decode_plate_thumbnails).

void SliceEngine::assign_arrange_order()
{
    int order = 1;
    for (ModelObject* obj : m_model.objects)
        for (ModelInstance* inst : obj->instances)
            inst->arrange_order = order++;
}

void SliceEngine::setup_extruder_params()
{
    int num_extruders = 0;
    if (m_config.has("filament_diameter"))
    {
        auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
        if (fd)
            num_extruders = static_cast<int>(fd->values.size());
    }
    Model::setExtruderParams(m_config, num_extruders);
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

static void decode_one_plate_thumbnail(PlateData& pd)
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

    // --- Identify this plate's instances and mark printability ---
    std::set<int> identify_ids = collect_plate_instance_ids(plate_id);
    if (!mark_printable_instances(identify_ids))
        return;

    // Plate origin = this plate's grid-layout offset. Shared by two downstream
    // steps: run_build_volume_check (translates instances into plate-local
    // coordinates) and prepare_plate_print (positions the plate in global space). Plate
    // dimensions are derived from printable_area inside setup_print_origin.
    Vec3d origin = setup_print_origin(plate_id);
    if (!run_build_volume_check(plate_id, identify_ids, origin))
        return;

    // --- Timeout check (before heavy slicing work) ---
    // Deliberately placed AFTER the cheap pre-stage checks
    // (collect_plate_instance_ids / mark_printable_instances /
    // setup_print_origin / run_build_volume_check), not at the top of
    // process_plate. within_slicing_deadline() depends only on
    // m_has_timeout / m_timeout_deadline (no local state), so moving it up
    // would be data-safe -- but those pre-stages also push diagnostics
    // (empty-plate / build-volume warnings) into m_stats. Keeping the
    // timeout gate just before the heavy Print work means those warnings
    // still land in the JSON alongside the SLICING_TIMEOUT error, instead
    // of being skipped. build_statistics() runs unconditionally in run(),
    // so the timeout error is emitted either way.
    if (!within_slicing_deadline(plate_id))
        return;

    // --- Create Print ---
    Print print;
    init_print(print);

    // --- Prepare plate print ---
    if (!prepare_plate_print(plate_id, print, origin))
        return;

    // --- Set print speed table (needs this plate's Print config) ---
    setup_print_speed_table(print);

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
    if (!all_gcode_layers_valid(slice_result))
    {
        remove_unusable_gcode(plate_id, slice_result);  // discard: delete the unusable G-code file
        finalise_plate_result(plate_id, slice_result); // archive: mirrors the normal path (issues left for build_statistics)
        return;
    }

    // --- Post-processing ---
    do_postprocessing(plate_id, slice_result, Vec2d(origin.x(), origin.y()));

    // --- Finalise ---
    finalise_plate_result(plate_id, slice_result);
}

// ============================================================================
// Per-plate sub-stages (in call order)
// ============================================================================

std::set<int> SliceEngine::collect_plate_instance_ids(int plate_id) const
{
    std::set<int> ids;
    for (const auto& pd : m_plate_data)
    {
        if (pd->plate_index == plate_id)
        {
            for (const auto& [object_id, inst_info] : pd->obj_inst_map)
            {
                ids.insert(inst_info.second);
            }
            break;
        }
    }
    return ids;
}

bool SliceEngine::mark_printable_instances(const std::set<int>& plate_ids)
{
    int count = 0;
    for (ModelObject* obj : m_model.objects)
    {
        for (ModelInstance* inst : obj->instances)
        {
            bool on_plate = (plate_ids.find(static_cast<int>(inst->loaded_id)) != plate_ids.end());
            inst->printable = on_plate;
            inst->print_volume_state = on_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
            if (on_plate)
                ++count;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Marked " << count << " instances printable";

    if (count == 0)
    {
        BOOST_LOG_TRIVIAL(warning) << "Skipping empty plate";
        return false;
    }
    return true;
}

Slic3r::Vec3d SliceEngine::setup_print_origin(int plate_id)
{
    // Derive plate dimensions from printable_area. Guarded the same way as
    // run_build_volume_check: if printable_area is absent/empty, return a zero
    // origin -- the build-volume check short-circuits on the same condition, so
    // the origin is never consumed in that case.
    const auto* pa = m_config.option<ConfigOptionPoints>("printable_area");
    if (!pa || pa->values.empty())
        return Vec3d::Zero();
    BoundingBoxf bbox;
    for (const Vec2d& pt : pa->values)
        bbox.merge(pt);
    double plate_width  = bbox.size().x();
    double plate_depth  = bbox.size().y();

    // Grid-layout origin (row-major, LOGICAL_PART_PLATE_GAP spacing). Pure
    // arithmetic lives in orca::compute_plate_origin (unit-tested); this thin
    // wrapper only bridges m_config/m_plate_data to its arguments.
    int total_plates = static_cast<int>(m_plate_data.size());
    orca::PlateOrigin o = orca::compute_plate_origin(plate_id, total_plates, plate_width, plate_depth);

    return Vec3d(o.x, o.y, 0.0);
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
                // The instance offsets were shifted to plate-local above, so
                // instance_bounding_box returns plate-local coordinates here.
                BoundingBoxf3 wb = obj->instance_bounding_box(*inst);
                push_build_volume_issues(plate_id, obj->name, wb, build_volume);
                log_plate_message("[Pre-processing]", "ERROR", plate_id,
                                  "Object \"" + obj->name +
                                      "\" exceeds the build volume (see issues for direction).");
                has_partly_outside = true;
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

void SliceEngine::push_build_volume_issues(int plate_id, const std::string& object_name,
                                           const BoundingBoxf3& bbox, const BuildVolume& build_volume)
{
    // Directional subdivision of a Partly_Outside instance. The build_volume
    // itself already concluded the instance collides with the volume boundary
    // (calc_print_volume_state); here we classify WHICH axis, so the JSON
    // consumer can act (raise / lower / reposition) without re-deriving it.
    //
    // Pure classification lives in orca::classify_build_volume_issues
    // (unit-tested). This thin wrapper only bridges the libslic3r types
    // (BoundingBoxf3 / BuildVolume) to the raw-double inputs the pure function
    // expects, then folds the results into m_stats.issues.
    const BoundingBoxf bed2d = build_volume.bounding_volume2d();
    auto issues = orca::classify_build_volume_issues(
        plate_id, object_name,
        bbox.min.x(), bbox.max.x(),
        bbox.min.y(), bbox.max.y(),
        bbox.min.z(), bbox.max.z(),
        build_volume.printable_height(),
        bed2d.min.x(), bed2d.max.x(),
        bed2d.min.y(), bed2d.max.y());
    for (auto& issue : issues)
        m_stats.issues.push_back(std::move(issue));
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

bool SliceEngine::within_slicing_deadline(int plate_id)
{
    // Pure time comparison lives in orca::deadline_expired (unit-tested). The
    // m_has_timeout guard is essential: with no timeout requested,
    // m_timeout_deadline is a default (epoch) time_point and must never trigger
    // an expiry. Sample now() once to match the original single-read semantics.
    auto now = std::chrono::steady_clock::now();
    if (!(m_has_timeout && orca::deadline_expired(now, m_timeout_deadline)))
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

bool SliceEngine::prepare_plate_print(int plate_id, Print& print, const Vec3d& origin)
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
    // Multi-extruder / wipe-tower handling is delegated entirely to libslic3r.
    // Previous revisions trimmed the per-filament config arrays down to a single
    // slot when the engine judged a plate "single-extruder", to stop
    // Print::has_wipe_tower() (which keys off filament_diameter.size()) from
    // generating a wipe-tower tool-change sequence that didn't match actual
    // extrusion ("append_tcr was asked to do a toolchange it didn't expect").
    //
    // That trim has been removed. The crash it guarded against never fires when
    // the wipe-tower decision is left to libslic3r's own ToolOrdering, which
    // counts extruders per-layer from real geometry (Print::object_extruders,
    // collect_extruders, custom_gcode_per_print_z, layer_config_ranges) rather
    // than from the configured slot count. A multi-slot project whose plate uses
    // only one colour simply yields has_wipe_tower()==false and no tool changes.
    //
    // Removing the trim also fixes two misclassifications it introduced:
    //   - a plate whose colour assignment lives in layer_config_ranges was read
    //     as single-extruder (the engine's own used_extruders collection never
    //     consulted that source) and wrongly trimmed to one colour;
    //   - the trim's slot remap rewrote the actually-used slot to index 0,
    //     so stats reported fil1 regardless of which slot the model used.
    // Both are now reported correctly because m_config reaches Print::apply
    // intact. Verified across 7 models (single-colour, modifier multi-colour,
    // AMS per-layer, height-range, vase, by-object, empty-layer).
    //
    // Work on a per-plate copy so per-plate overrides below do not leak into
    // subsequent plates (m_config is shared across the pipeline). Key backfill
    // and flush-matrix normalization are done once on m_config by
    // normalize_loaded_config() before the per-plate loop.
    DynamicPrintConfig merged_config = m_config;

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

void SliceEngine::setup_print_speed_table(Print& print)
{
    Model::setPrintSpeedTable(m_config, print.config());
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

    // Pure classification (warning-path code table) lives in
    // orca::classify_validate_exception (unit-tested). NOTE: the ORGANIC type
    // escalates to error level on the warning path too.
    auto cls = orca::classify_validate_exception(static_cast<int>(warning.type), /*is_error_path=*/false, warning.string);
    std::string message = cls.fixed_message.empty() ? (warning.string + opt_hint) : cls.fixed_message;

    if (cls.level == IssueLevel::error)
    {
        std::string suggestion = cls.fixed_message.empty() ? std::string{} : orca::kOrganicFixedSuggestion;
        m_stats.issues.push_back(make_error(plate_id, cls.code, message, obj_name, suggestion));
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
    }
    else
    {
        m_stats.issues.push_back(make_warning(plate_id, cls.code, message, obj_name));
    }
}

bool SliceEngine::emit_validate_error(int plate_id, const StringObjectException& err)
{
    auto [obj_name, opt_hint] = format_exception_context(err);
    // Pure classification (error-path code table, NOT_DEFINED substring match,
    // continue/abort contract) lives in orca::classify_validate_exception
    // (unit-tested). This thin wrapper adds logging, Issue construction and the
    // member side effects (m_any_error / set_error_type).
    auto cls = orca::classify_validate_exception(static_cast<int>(err.type), /*is_error_path=*/true, err.string);

    // NOT_DEFINED (continue_slicing=true) is logged at warning level, matching
    // the original code; all other error-path types are logged at error level.
    // (BOOST_LOG_TRIVIAL needs a literal severity name, so branch explicitly.)
    if (cls.continue_slicing)
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;
    else
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;

    std::string message = cls.fixed_message.empty() ? (err.string + opt_hint) : cls.fixed_message;
    if (cls.level == IssueLevel::error)
    {
        std::string suggestion = cls.fixed_message.empty() ? std::string{} : orca::kOrganicFixedSuggestion;
        m_stats.issues.push_back(make_error(plate_id, cls.code, message, obj_name, suggestion));
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
    }
    else
    {
        m_stats.issues.push_back(make_warning(plate_id, cls.code, message, obj_name));
    }
    return cls.continue_slicing;
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
    // NOTE: Per-plate filtering relies on mark_printable_instances(plate_ids)
    // in process_plate having marked off-plate instances printable=false.
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

        auto thumbnail_cb = [this](const Slic3r::ThumbnailsParams& params) {
            return make_plate_thumbnails(params);
        };

        std::string exported = print.export_gcode(gcode_output, &result.gcode_result, thumbnail_cb);
        result.gcode_path = exported;

        // Post-processing scripts are disabled in cloud mode to prevent
        // remote code execution via user-uploaded 3MF files.
        // run_post_process_scripts(result.gcode_path, print.full_print_config());

        collect_print_warnings(plate_id, print, result);

        const PrintStatistics& ps = print.print_statistics();
        result.total_weight = ps.total_weight;
        result.support_used = print.is_support_used();
        result.total_used_filament = ps.total_used_filament;
        result.total_cost = ps.total_cost;
        result.filament_volumes = result.gcode_result.print_statistics.total_volumes_per_extruder;

        capture_post_trim_config_snapshot(print, result);

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

std::vector<ThumbnailData> SliceEngine::make_plate_thumbnails(const ThumbnailsParams& params) const
{
    std::vector<ThumbnailData> thumbnails;
    const ThumbnailData* source = nullptr;
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
}

void SliceEngine::collect_print_warnings(int plate_id, Print& print, PlateSliceResult& result)
{
    // message_id-aware grading. Desktop CLI treats EmptyGcodeLayers and
    // GcodeOverlap as CLI_SLICING_ERROR (hard exit). The cloud engine flags the
    // plate but continues processing remaining plates.
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
}

void SliceEngine::capture_post_trim_config_snapshot(Print& print, PlateSliceResult& result)
{
    // print.config() is the merged config the slicer actually applied: m_config plus
    // FullPrintConfig::defaults() backfill plus per-plate overrides, normalised by
    // print.apply(). This is the source of truth for the "; filament_colour" /
    // "; filament_type" headers emitted in the G-code. The downstream stats/export
    // consumers (populate_plate_data_for_export, assemble_plate_stats) run post-slice
    // with no Print object in scope, so the only way they can read print.config() is
    // via this snapshot. m_config itself is NOT equivalent — it predates apply(), so
    // it lacks the backfill/override normalisation and can diverge even without any
    // per-extruder remap. Stash by value so consumers read the slice-accurate values.
    if (print.config().has("filament_colour")) {
        const auto* fc = print.config().option<ConfigOptionStrings>("filament_colour");
        if (fc) result.filament_colours = fc->values;
    }
    if (print.config().has("filament_type")) {
        const auto* ft = print.config().option<ConfigOptionStrings>("filament_type");
        if (ft) result.filament_types = ft->values;
    }
    // Same rationale as colours/types: snapshot the nozzle / filament diameters /
    // densities from the post-apply print.config() so downstream stats compute
    // length/weight against the values the slicer actually used, not the un-merged
    // m_config.
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
}

// Guard: returns true when every G-code layer is valid, i.e. export_gcode
// produced no PRINT_EMPTY_GCODE_LAYERS issue. Pure query of slice_result -- no
// member state. File-local free function.
static bool all_gcode_layers_valid(const PlateSliceResult& slice_result)
{
    // EmptyGcodeLayers means the plate has no valid layers; the G-code file
    // exists but is effectively empty. export_gcode pushes this issue during
    // the slice pipeline; here we only read the flag (pure query).
    for (const auto& iss : slice_result.issues)
    {
        if (iss.code == "PRINT_EMPTY_GCODE_LAYERS")
            return false;
    }
    return true;
}

// Remove the G-code file produced for a plate whose layers are all invalid
// (empty-layer plate) -- the file is unusable, so delete it rather than ship it.
// Only removes the file (plate_id is just for the log line); issues stay in
// slice_result for finalise_plate_result + build_statistics to collect. File-local
// free function: no member state.
static void remove_unusable_gcode(int plate_id, PlateSliceResult& slice_result)
{
    boost::filesystem::remove(slice_result.gcode_path);
    BOOST_LOG_TRIVIAL(warning) << "Plate " << (plate_id + 1)
                               << ": empty G-code layers, G-code file discarded";
}

// Compute gcode_result.toolpath_outside, replicating desktop
// GCodeViewer::load_toolpaths (GCodeViewer.cpp:2354-2419). On desktop this flag
// is produced by the GUI's GCodeViewer and only consumed afterwards; the
// headless export path never sets it, so without this port the
// TOOLPATH_OUTSIDE check below reads a field that is always false. Two
// headless-specific adaptations:
//  - the bed polygon comes from the engine config (GCodeProcessor's
//    apply_config(const PrintConfig&) at GCodeProcessor.cpp:794 fills
//    printable_height but never printable_area);
//  - moves are stored in global grid-layout coordinates (store_move_vertex
//    adds the plate offset back, GCodeProcessor.cpp:4797) while the written
//    G-code text is plate-local, so the check shifts the bounding box into
//    plate-local space first — same as the desktop viewer renders each plate.
static void compute_toolpath_outside(GCodeProcessorResult& gcode_result, const DynamicPrintConfig& config,
                                     const Slic3r::Vec2d& plate_origin)
{
    const auto* printable_area_opt = config.option<ConfigOptionPoints>("printable_area");
    if (!printable_area_opt || printable_area_opt->values.empty())
        return; // no bed defined — nothing to check against

    // Shift moves into plate-local coordinates (see comment above) so that
    // both the bbox-based Rectangle branch and the per-move Circle/Convex
    // branches of all_paths_inside see the same plate-local space. moves
    // are visualization-only data the engine drops right after this check
    // (finalise_plate_result), so in-place mutation is safe.
    if (plate_origin.squaredNorm() > 0.0) {
        const Vec3f shift(-static_cast<float>(plate_origin.x()),
                          -static_cast<float>(plate_origin.y()), 0.0f);
        for (auto& move : gcode_result.moves)
            move.position += shift;
    }

    // Desktop m_paths_bounding_box: Extrude moves only (custom-gcode moves and
    // zero-width/height moves excluded), arc interpolation points included.
    BoundingBoxf3 paths_bbox;
    Points pts;
    auto merge = [&paths_bbox, &pts](const Vec3f& position) {
        paths_bbox.merge(position.cast<double>());
        pts.emplace_back(Point(scale_(position.x()), scale_(position.y())));
    };
    for (const auto& move : gcode_result.moves) {
        if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom &&
            move.width != 0.0f && move.height != 0.0f) {
            merge(move.position);
            if (move.is_arc_move_with_interpolation_points())
                for (const auto& p : move.interpolation_points)
                    if (move.width != 0.0f && move.height != 0.0f)
                        merge(p);
        }
    }
    if (paths_bbox.defined) {
        const BuildVolume build_volume(printable_area_opt->values, gcode_result.printable_height);
        bool contained = build_volume.all_paths_inside(gcode_result, paths_bbox);
        if (contained && !gcode_result.bed_exclude_area.empty()) {
            // Desktop checks the toolpath convex hull against each bed exclude
            // area polygon (GCodeViewer.cpp:2404-2417); the exclude bounding
            // boxes are built from 4-point groups (PartPlate.cpp:386-403).
            const Polygon convex_hull_2d = Geometry::convex_hull(std::move(pts));
            BoundingBoxf3 exclude_bb;
            for (size_t i = 0; i < gcode_result.bed_exclude_area.size(); ++i) {
                if (i % 4 == 0)
                    exclude_bb = BoundingBoxf3();
                const Vec2d& p = gcode_result.bed_exclude_area[i];
                exclude_bb.merge(Vec3d(p(0), p(1), 0.0));
                if (i % 4 == 3 &&
                    !intersection({ exclude_bb.polygon(true) }, { convex_hull_2d }).empty()) {
                    contained = false;
                    break;
                }
            }
        }
        gcode_result.toolpath_outside = !contained;
    }
}

void SliceEngine::do_postprocessing(int plate_id, PlateSliceResult& result, const Slic3r::Vec2d& plate_origin)
{
    bool has_postprocess_error = false;
    bool has_postprocess_warning = false;

    // Toolpaths outside print volume. Desktop blocks printing via
    // is_slice_result_ready_for_print() when toolpath_outside is true.
    compute_toolpath_outside(result.gcode_result, m_config, plate_origin);
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
        // Per-plate nozzle string: prefer the snapshot (the post-apply print.config() value
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
            // Prefer the types captured at slice time (result.filament_types), which mirror
            // the post-apply print.config() values written to the G-code header. m_config is
            // the raw pre-apply array, which lacks backfill/per-plate overrides and (on a
            // single-extruder plate) may have its index 0 point at a different slot.
            if (idx < result.filament_types.size())
                info.type = result.filament_types[idx];
            else if (filament_types && idx < filament_types->values.size())
                info.type = filament_types->values[idx];
            // Use the colours captured at slice time (result.filament_colours), which match
            // the G-code header. m_config holds the raw pre-apply array, which lacks
            // backfill/per-plate overrides and (on a single-extruder plate) may have its
            // index 0 point at a different slot.
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
    // Diagnostic flags are meaningful regardless of plate success (a plate
    // fails precisely BECAUSE toolpath_outside fired) — fill them before the
    // early return, unlike the stats below which need a successful slice.
    plate_stats.toolpath_outside = result.gcode_result.toolpath_outside;
    plate_stats.long_retraction_when_cut = result.gcode_result.long_retraction_when_cut;

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

    // Prefer the diameters/densities captured at slice time (result.filament_*), which
    // mirror the post-apply print.config() the slicer applied. gcode_result mirrors the
    // same merged config but is kept as a fallback for paths that don't populate the
    // snapshot (e.g. failed plates).
    // Note: result.filament_* are std::vector<double> (from ConfigOptionFloats) while
    // gcode_result.filament_* are std::vector<float> (libslic3r GCodeProcessorResult), so the
    // snapshot-vs-fallback choice must stay as a runtime branch, not a ?: over mismatched types.
    const auto& fd_snapshot = result.filament_diameters;        // vector<double>
    const auto& fdens_snapshot = result.filament_densities;     // vector<double>
    const auto& fd_fallback = result.gcode_result.filament_diameters;    // vector<float>
    const auto& fdens_fallback = result.gcode_result.filament_densities; // vector<float>
    bool use_snapshot = !fd_snapshot.empty() && !fdens_snapshot.empty();

    // Lookup helpers: return the per-extruder diameter/density as double from whichever
    // source is active, with an out-of-range guard (size_t idx, never negative here).
    auto diameter_of = [&](size_t i) -> double {
        return use_snapshot ? (i < fd_snapshot.size() ? fd_snapshot[i] : 0.0)
                            : (i < fd_fallback.size() ? fd_fallback[i] : 0.0);
    };
    auto density_of = [&](size_t i) -> double {
        return use_snapshot ? (i < fdens_snapshot.size() ? fdens_snapshot[i] : 0.0)
                            : (i < fdens_fallback.size() ? fdens_fallback[i] : 0.0);
    };

    for (const auto& [extruder_id, volume] : result.filament_volumes)
    {
        double diameter = diameter_of(extruder_id);
        double density = density_of(extruder_id);
        if (diameter <= 0.0)
            continue;
        double cross_section = M_PI * 0.25 * diameter * diameter;
        double used_m = (volume / cross_section) * 0.001;
        double used_g = volume * density * 0.001;
        plate_stats.filament_used_m[extruder_id] = used_m;
        plate_stats.filament_used_g[extruder_id] = used_g;
    }

    double total_support_m = 0.0, total_support_g = 0.0;
    double total_flush_m = 0.0, total_flush_g = 0.0;
    double total_wipe_tower_m = 0.0, total_wipe_tower_g = 0.0;

    auto& ps = result.gcode_result.print_statistics;
    auto accumulate = [&](const std::map<size_t, double>& volumes, double& m_acc, double& g_acc)
    {
        for (const auto& [extruder_id, volume] : volumes)
        {
            double diameter = diameter_of(extruder_id);
            if (diameter <= 0.0)
                continue;
            double density = density_of(extruder_id);
            double cross_section = M_PI * 0.25 * diameter * diameter;
            m_acc += (volume / cross_section) * 0.001;
            g_acc += volume * density * 0.001;
        }
    };
    accumulate(ps.support_volumes_per_extruder, total_support_m, total_support_g);
    accumulate(ps.wipe_tower_volumes_per_extruder, total_wipe_tower_m, total_wipe_tower_g);
    accumulate(ps.flush_per_filament, total_flush_m, total_flush_g);

    plate_stats.model_filament_m =
        plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
    plate_stats.model_filament_g =
        plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;

    // Prefer the nozzle diameters captured at slice time (the post-apply print.config()
    // values); fall back to the raw m_config (which still has N slots) for paths that
    // never populated the snapshot.
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
    // m_config. prepare_merged_config_for_plate builds the per-plate merged config
    // (m_config + backfill + per-plate override, plus a possible single-extruder remap
    // that keeps slot keep_idx → 0), so m_config's index 0 can point at a different
    // colour than the one in the emitted G-code. result.filament_colours holds the
    // post-apply values captured in export_gcode from print.config() — read from there
    // first, fall back to m_config.
    const auto& result_colors = result.filament_colours;
    // Filament types captured at slice time — same rationale as the colours: m_config's
    // index 0 may point at a different slot after single-extruder remap, so read the
    // values that actually went into the G-code first, fall back to m_config.
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
            detail.type = result_types[ext_sz];
        else if (ftypes && ext_sz < ftypes->values.size())
            detail.type = ftypes->values[ext_sz];
        else
            detail.type = "Unknown";

        if (ext_sz < result_colors.size())
            detail.color = result_colors[ext_sz];
        else if (fcolors && ext_sz < fcolors->values.size())
            detail.color = fcolors->values[ext_sz];
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
