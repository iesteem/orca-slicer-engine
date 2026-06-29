#include "SliceEngine.hpp"
#include "GeometryCheck.hpp"
#include "Utils.hpp"

#include "PresetManager.hpp"
#include "PlateProcessor.hpp"
#include "StatisticsBuilder.hpp"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>

#include <cctype>
#include <climits>
#include <cstdio>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

#include <tbb/global_control.h>

#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/CustomGCode.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

constexpr int MAX_RETRIES = 3;

using namespace Slic3r;

namespace {

// 1/5, same as GUI's LOGICAL_PART_PLATE_GAP
constexpr double LOGICAL_PART_PLATE_GAP = 0.2;

// RAII guard to temporarily adjust boost::log severity threshold.
// Uses a static depth counter so nested guards don't fight over the
// filter.  On outermost exit the filter is restored to >= info so
// engine progress markers remain visible while library trace/debug
// stay suppressed.
class ScopedLogLevel {
    static int s_depth;
public:
    explicit ScopedLogLevel(boost::log::trivial::severity_level lv) {
        if (s_depth++ == 0) {
            namespace expr = boost::log::expressions;
            boost::log::core::get()->set_filter(
                expr::attr<boost::log::trivial::severity_level>("Severity") >= lv
            );
        }
    }
    ~ScopedLogLevel() {
        if (--s_depth == 0) {
            namespace expr = boost::log::expressions;
            boost::log::core::get()->set_filter(
                expr::attr<boost::log::trivial::severity_level>("Severity")
                >= boost::log::trivial::info
            );
        }
    }
    ScopedLogLevel(const ScopedLogLevel&) = delete;
    ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;
};

int ScopedLogLevel::s_depth = 0;

// --- Official preset file helpers ---

// Directory constants for system preset files under resources/profiles/
static const char* const SNAPMK_FILAMENT_DIR = "/profiles/Snapmaker/filament/";
static const char* const ORCA_FILAMENT_DIR() { // ORCA_FILAMENT_LIBRARY is a runtime string
    static const std::string s = std::string("/profiles/") + PresetBundle::ORCA_FILAMENT_LIBRARY + "/filament/";
    return s.c_str();
}

// Check whether a filament name matches an official preset on disk
// (Snapmaker or OrcaFilamentLibrary, including @System suffix for Generic filaments).
inline bool is_official_filament_file(const std::string& preset_name)
{
    const std::string& res = Slic3r::resources_dir();
    if (res.empty()) return false;
    const std::string snap_path = res + SNAPMK_FILAMENT_DIR + preset_name + ".json";
    const std::string orca_dir  = res + ORCA_FILAMENT_DIR();
    return boost::filesystem::exists(snap_path)
        || boost::filesystem::exists(orca_dir + preset_name + ".json")
        || boost::filesystem::exists(orca_dir + preset_name + " @System.json");
}

// Check if a plate result indicates a wipe tower tool change mismatch.
// CGAL/float differences on some platforms cause non-consecutive extruder
// ID handling to fail during G-code export.
bool is_wipe_tower_error(const PlateSliceResult& result)
{
    for (const auto& iss : result.issues) {
        if (iss.message.find("append_tcr") != std::string::npos)
            return true;
    }
    return false;
}

bool has_no_layers_on_plate(const std::vector<Issue>& issues, int plate_id)
{
    for (const auto& iss : issues) {
        if (iss.plate_id == plate_id &&
            iss.code == "SLICING_ERROR" &&
            iss.message.find("No layers to export") != std::string::npos)
            return true;
    }
    return false;
}

// Check if the plate has a fatal slicing error (geometry-level failure).
// Unlike "no layers" (which is model placement), SlicingError/SlicingErrors
// from print.process() are deterministic — retrying with the same input
// will always produce the same failure.  Skip retry to save CPU.
bool has_fatal_slicing_error_on_plate(const std::vector<Issue>& issues, int plate_id)
{
    for (const auto& iss : issues) {
        if (iss.plate_id == plate_id &&
            iss.code == "SLICING_ERROR" &&
            iss.message.find("No layers to export") == std::string::npos)
            return true;
    }
    return false;
}

// Named constants for magic numbers
constexpr const char* BED_TEMP_WARNING_CODE = "1000C001";

} // namespace

SliceEngine::~SliceEngine() = default;

SliceEngine::SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files)
    : m_cfg(cfg)
    , m_temp_files(temp_files)
{
    // Build the shared context AFTER all members are initialized so
    // references remain stable for the engine's lifetime.
    m_ctx = std::make_unique<EngineContext>(EngineContext{
        m_cfg,
        m_temp_files,
        m_model,
        m_config,
        m_stats,
        m_plate_data,
        m_project_presets,
        m_plate_results,
        m_output_path,
        m_any_error,
        m_any_postprocess_warning,
        m_error_type,
        m_baked_instance_z,
        m_has_timeout,
        m_timeout_deadline,
    });

    // Create sub-components
    m_presets       = std::make_unique<PresetManager>(*m_ctx);
    m_plate_proc    = std::make_unique<PlateProcessor>(*m_ctx);
    m_stats_builder = std::make_unique<StatisticsBuilder>(*m_ctx);
}

bool SliceEngine::run() {
    // Pre-compute output path so JSON is written to the correct filename
    // even when the slicing pipeline returns early (e.g., filament validation).
    m_output_path = generate_output_path(m_cfg.input_file, m_cfg.output_base,
                                         m_cfg.plate_id, m_cfg.format, m_cfg.single_plate);

    m_stats.issues.reserve(32);

    try {

    // --- Config layering & mutation order ---
    // 1. FullPrintConfig::defaults()    — baseline for all keys
    // 2. load_3mf()                     — overlay 3MF project + embedded presets
    // 3. PresetManager                  — system presets + validation
    // 4. validate_printer_model()       — verify printer_model == "Snapmaker U1"
    // 5. PresetManager::apply_printer_official_preset() — overwrite from official
    // 6. validate_filament_official()   — MAY replace filament_settings_id
    m_config.apply(FullPrintConfig::defaults());

    bool load_ok = load_3mf();
    if (!load_ok) {
        m_stats_builder->build_statistics();
        return false;
    }

    sanitize_config();
    validate_config();

    {
        ScopedLogLevel quiet(boost::log::trivial::warning);
        m_presets->load_system_presets();
        m_presets->validate_presets();
    }

    if (!m_presets->validate_printer_model()) {
        m_stats_builder->build_statistics();
        return false;
    }
    m_presets->apply_printer_official_preset();

    if (!m_presets->validate_filament_official(m_cfg.substitute_filaments)) {
        m_stats_builder->build_statistics();
        return false;
    }

    if (m_any_error) {
        m_stats_builder->build_statistics();
        return false;
    }

    bool validate_ok = validate_input();

    if (load_ok && validate_ok) {
        m_has_timeout = (m_cfg.timeout_seconds > 0);
        if (m_has_timeout) {
            m_timeout_deadline = std::chrono::steady_clock::now()
                               + std::chrono::seconds(m_cfg.timeout_seconds);
        }

        auto geom_issues = run_geometry_checks(m_model);
        for (auto& issue : geom_issues) {
            m_stats.issues.push_back(std::move(issue));
        }

        if (m_cfg.thread_count > 0) {
            m_tbb_control = std::make_unique<tbb::global_control>(
                tbb::global_control::max_allowed_parallelism, m_cfg.thread_count);
            BOOST_LOG_TRIVIAL(info) << "TBB thread count limited to " << m_cfg.thread_count;
        }

        std::vector<int> plates_to_process;
        if (m_cfg.single_plate) {
            plates_to_process.push_back(m_cfg.plate_id - 1);
        } else {
            for (const auto& pd : m_plate_data)
                plates_to_process.push_back(pd->plate_index);
        }

        if (plates_to_process.empty()) {
            BOOST_LOG_TRIVIAL(error) << "No plates to process";
            m_any_error = true;
            m_stats_builder->set_error_type(EXIT_PREPROCESS_ERROR);
            m_stats.error_message = "No plates found in the 3MF file to slice";
            m_stats.issues.push_back(make_error(-1, "NO_PLATES",
                "No plates found in the 3MF file to slice. "
                "Please add objects to at least one plate and try again."));
        } else {
            // Decode plate thumbnails from input .3mf (PNG→RGBA) so they
            // can be embedded into output gcode / .3mf packages.
            decode_plate_thumbnails();

            for (int plate_id : plates_to_process) {
                m_plate_proc->process_plate(plate_id);
            }
        }

        bool has_output = !m_plate_results.empty();
        if (has_output && !m_any_error && (m_cfg.format == OutputFormat::GCODE_3MF || !m_cfg.single_plate))
            m_stats_builder->package_output();

        if (m_any_error && m_cfg.single_plate && m_cfg.format == OutputFormat::GCODE) {
            boost::filesystem::remove(m_output_path);
            BOOST_LOG_TRIVIAL(info) << "Removed output file due to errors: " << m_output_path;
        }
    }

    m_stats_builder->build_statistics();

    return !m_plate_results.empty();

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Fatal exception in slicing pipeline: " << e.what();
        std::cerr << "[ERROR] An unexpected internal error occurred during slicing." << std::endl;
        m_any_error = true;
        m_stats_builder->set_error_type(EXIT_SLICING_ERROR);
        m_stats.issues.push_back(make_error(-1, "INTERNAL_ERROR",
            std::string("An unexpected internal error occurred: ") + e.what()));
        m_stats_builder->build_statistics();
        return false;
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Fatal non-standard exception in slicing pipeline";
        std::cerr << "[ERROR] An unexpected internal error occurred." << std::endl;
        m_any_error = true;
        m_stats_builder->set_error_type(EXIT_SLICING_ERROR);
        m_stats.issues.push_back(make_error(-1, "INTERNAL_ERROR",
            "Unhandled unknown exception in slicing pipeline"));
        m_stats_builder->build_statistics();
        return false;
    }
}

// ============================================================================
// Stage 1: Load 3MF
// ============================================================================

bool SliceEngine::load_3mf() {
    // --- Pre-load validation: format & size ---

    // Extension check
    std::string ext = boost::filesystem::path(m_cfg.input_file).extension().string();
    if (ext != ".3mf") {
        std::string msg = "Only .3mf print configuration files are supported. "
                          "Please export the file from Snapmaker Orca Slicer.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "FORMAT_REJECTED", msg));
        return false;
    }

    // File size check (configurable via --max-size, default 200MB, 0 = no limit)
    if (m_cfg.max_size_mb > 0) {
        boost::uintmax_t max_file_size = static_cast<boost::uintmax_t>(m_cfg.max_size_mb) * 1024ULL * 1024ULL;
        boost::system::error_code ec;
        boost::uintmax_t file_size = boost::filesystem::file_size(m_cfg.input_file, ec);
        if (!ec && file_size > max_file_size) {
            std::string msg = "File size exceeds the limit (" + std::to_string(m_cfg.max_size_mb)
                            + " MB). Please simplify the model or reduce the face count and retry.";
            BOOST_LOG_TRIVIAL(error) << msg;
            m_any_error = true;
            set_error_type(EXIT_LOAD_ERROR);
            m_stats.error_message = msg;
            m_stats.issues.push_back(make_error(-1, "FILE_SIZE_EXCEEDED", msg));
            return false;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Loading 3MF file...";

    // Reset substitution state from any prior run
    m_config_substitutions = ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule::Enable);

    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                           LoadStrategy::AddDefaultInstances | LoadStrategy::LoadAuxiliary;

    try {
        ScopedLogLevel quiet_load(boost::log::trivial::warning);
        m_model = Model::read_from_file(
            m_cfg.input_file,
            &m_config,
            &m_config_substitutions,
            strategy,
            &m_plate_data,
            &m_project_presets,
            &m_is_BBL_3mf,
            &m_file_version,
            nullptr,
            nullptr,
            nullptr,
            0
        );
    }
    catch (const std::exception& e) {
        std::string what = e.what();
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << what;

        // Detect gcode.3mf output files (no geometry, only pre-sliced G-code).
        // Model::read_from_file() throws "The supplied file couldn't be read
        // because it's empty" when the 3MF has valid XML metadata but zero
        // model objects — typical of a .gcode.3mf slicing result being
        // mistakenly re-submitted as input.
        bool is_empty = (what.find("empty") != std::string::npos);

        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);

        if (is_empty) {
            m_stats.error_message =
                "This 3MF file contains no 3D model objects. "
                "It appears to be a gcode.3mf slicing output file, not a project file. "
                "Please upload the original .3mf project file instead.";
            m_stats.issues.push_back(make_error(-1, "LOAD_3MF_ERROR", m_stats.error_message));
        } else {
            m_stats.error_message =
                "Failed to load 3MF file. The file may be corrupted or in an unsupported format.";
            m_stats.issues.push_back(make_error(-1, "LOAD_3MF_ERROR", m_stats.error_message));
        }

        // Model::read_from_file() may have partially populated output parameters
        // (plate data, project presets) before throwing.  Clear them to prevent
        // downstream code from accessing invalid state.
        m_plate_data.clear();
        m_project_presets.clear();

        return false;
    }

    if (m_model.objects.empty()) {
        BOOST_LOG_TRIVIAL(error) << "No objects found in 3MF file";
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = "3MF file contains no sliceable model objects";
        m_stats.issues.push_back(make_error(-1, "MODEL_EMPTY", m_stats.error_message));
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Loaded " << m_model.objects.size() << " object(s)";

    // Detect and reject post-processing scripts in cloud mode (RCE prevention)
    if (m_config.has("post_process")) {
        auto pp = m_config.option<ConfigOptionStrings>("post_process", true);
        if (pp && !pp->values.empty()) {
            m_stats.issues.push_back(make_error(-1, "POST_PROCESS_REJECTED",
                "Custom post-processing scripts are not supported in cloud slicing."));
            m_config.set_key_value("post_process", new ConfigOptionStrings({}));
        }
    }

    return true;
}

void SliceEngine::decode_plate_thumbnails()
{
    for (auto& pd : m_plate_data) {
        if (pd->plate_thumbnail.pixels.empty())
            continue;

        Slic3r::png::ReadBuf buf{
            pd->plate_thumbnail.pixels.data(),
            pd->plate_thumbnail.pixels.size()
        };

        Slic3r::png::ImageColorscale img;
        if (!Slic3r::png::decode_colored_png(buf, img))
            continue;

        pd->plate_thumbnail.set(static_cast<unsigned int>(img.cols),
                                static_cast<unsigned int>(img.rows));

        const size_t src_bpp = static_cast<size_t>(img.bytes_per_pixel);
        for (size_t y = 0; y < img.rows; ++y) {
            for (size_t x = 0; x < img.cols; ++x) {
                size_t src_idx = (y * img.cols + x) * src_bpp;
                size_t dst_idx = (y * img.cols + x) * 4;
                pd->plate_thumbnail.pixels[dst_idx + 0] = img.buf[src_idx + 0];
                pd->plate_thumbnail.pixels[dst_idx + 1] = img.buf[src_idx + 1];
                pd->plate_thumbnail.pixels[dst_idx + 2] = img.buf[src_idx + 2];
                pd->plate_thumbnail.pixels[dst_idx + 3] =
                    (src_bpp >= 4) ? img.buf[src_idx + 3] : 255;
            }
        }
    }
}

void SliceEngine::sanitize_config()
{
    const PrintConfigDef* def = &Slic3r::print_config_def;
    std::vector<std::string> keys_to_erase;

    for (auto it = m_config.cbegin(); it != m_config.cend(); ++it) {
        const std::string& key = it->first;
        const ConfigOption* opt = it->second.get();
        const ConfigOptionDef* optdef = def->get(key);
        if (!optdef) continue;

        bool below_min = false;
        switch (opt->type()) {
        case coInt: {
            auto* iopt = static_cast<const ConfigOptionInt*>(opt);
            below_min = (iopt->value < optdef->min);
            break;
        }
        case coInts: {
            for (int v : static_cast<const ConfigOptionInts*>(opt)->values)
                if (v < optdef->min) { below_min = true; break; }
            break;
        }
        case coFloat:
        case coPercent:
        case coFloatOrPercent: {
            auto* fopt = static_cast<const ConfigOptionFloat*>(opt);
            below_min = (fopt->value < optdef->min);
            break;
        }
        case coFloats:
        case coPercents: {
            for (double v : static_cast<const ConfigOptionFloats*>(opt)->values)
                if (v < optdef->min) { below_min = true; break; }
            break;
        }
        default: break;
        }

        if (below_min) {
            keys_to_erase.push_back(key);
            m_stats.issues.push_back(make_warning(-1, "CONFIG_SANITIZED",
                key + " = " + opt->serialize()
                + " is below minimum (" + std::to_string(optdef->min)
                + "), reset to system default"));
        }
    }

    for (const std::string& key : keys_to_erase)
        m_config.erase(key);
}

// ============================================================================
// Stage 2: Config & preset validation (desktop parity)
// ============================================================================

void SliceEngine::validate_config()
{
    // A1: Validate config values (layer_height, nozzle_diameter, etc.)
    // Use under_cli=false to match desktop GUI behavior — invalid config
    // values produce warnings but do NOT block slicing.
    std::map<std::string, std::string> invalid = m_config.validate(false);
    if (!invalid.empty()) {
        for (const auto& [key, msg] : invalid)
            m_stats.issues.push_back(make_warning(-1, "CONFIG_INVALID_" + key, msg));
    }

    // A2: Check config substitutions (unknown keys, forward-compat changes)
    if (!m_config_substitutions.empty()) {
        for (const auto& sub : m_config_substitutions.substitutions) {
            const char* key = sub.opt_def ? sub.opt_def->opt_key.c_str() : "?";
            m_stats.issues.push_back(make_warning(-1, "CONFIG_SUBSTITUTION",
                std::string("Config key '") + key
                + "' value was substituted (old: " + sub.old_value + ")"));
        }

        for (const auto& key : m_config_substitutions.unrecogized_keys) {
            m_stats.issues.push_back(make_warning(-1, "CONFIG_UNRECOGNIZED",
                std::string("Unrecognized config key '") + key
                + "' — may be from a newer slicer version"));
        }
    }
}

void SliceEngine::load_system_presets()
{
    // Always derive profiles path from resources_dir/profiles/
    const std::string res_dir = Slic3r::resources_dir();
    if (res_dir.empty()) {
        BOOST_LOG_TRIVIAL(info) << "No resources directory set; skipping preset validation";
        return;
    }
    std::string profiles_path = res_dir + "/profiles";

    boost::filesystem::path profiles_dir(profiles_path);
    if (!boost::filesystem::exists(profiles_dir) ||
        !boost::filesystem::is_directory(profiles_dir)) {
        BOOST_LOG_TRIVIAL(warning)
            << "Profiles directory not found: " << profiles_dir.string()
            << "; skipping preset validation";
        return;
    }

    // Collect vendor JSON files
    std::vector<std::string> vendor_names;
    for (auto& entry : boost::filesystem::directory_iterator(profiles_dir)) {
        std::string file = entry.path().string();
        if (!Slic3r::is_json_file(file))
            continue;
        std::string name = entry.path().filename().string();
        name.erase(name.size() - 5); // strip .json
        vendor_names.push_back(name);
    }

    if (vendor_names.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "No vendor JSON files in " << profiles_dir.string()
            << "; skipping preset validation";
        return;
    }

    // Move OrcaFilamentLibrary to the front (cross-vendor filament inheritance)
    for (size_t i = 0; i < vendor_names.size(); ++i) {
        if (vendor_names[i] == PresetBundle::ORCA_FILAMENT_LIBRARY) {
            std::swap(vendor_names[0], vendor_names[i]);
            break;
        }
    }

    try {
        m_preset_bundle = std::make_unique<Slic3r::PresetBundle>();

        // In validation mode, load_vendor_configs_from_json reads presets
        // into this PresetBundle. We load all vendors directly (no merge_presets
        // needed — duplicate detection is not critical for cloud validation).
        const auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;

        // Only load Snapmaker + OrcaFilamentLibrary — the two vendors
        // whose presets are relevant for cloud engine validation.
        // Loading additional vendors clears Snapmaker presets that are
        // incompatible with non-Snapmaker printers.
        {
            // Load OrcaFilamentLibrary first (generic filament base),
            // then Snapmaker on top with inheritance from Orca.
            const std::string vendors[] = { PresetBundle::ORCA_FILAMENT_LIBRARY, "Snapmaker" };
            for (const auto& vendor : vendors) {
                m_preset_bundle->load_vendor_configs_from_json(
                    profiles_dir.string(), vendor,
                    PresetBundle::LoadSystem, rule, nullptr);
            }
        }

        m_presets_available = true;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "Failed to load system presets: " << e.what()
            << "; preset validation skipped";
        m_preset_bundle.reset();
        m_presets_available = false;
    }
}

void SliceEngine::validate_presets()
{
    if (!m_presets_available || !m_preset_bundle) {
        BOOST_LOG_TRIVIAL(info) << "Preset validation skipped (system presets not available)";
        return;
    }

    PresetBundle& bundle = *m_preset_bundle;

    // B2: Load project embedded presets
    if (!m_project_presets.empty()) {
        try {
            PresetsConfigSubstitutions preset_subs =
                bundle.load_project_embedded_presets(
                    m_project_presets,
                    ForwardCompatibilitySubstitutionRule::Enable);

            for (const auto& ps : preset_subs) {
                for (const auto& sub : ps.substitutions) {
                    const char* key = sub.opt_def ? sub.opt_def->opt_key.c_str() : "?";
                    m_stats.issues.push_back(make_warning(-1, "PRESET_SUBSTITUTION",
                        std::string("Embedded preset '") + ps.preset_name
                        + "' key '" + key + "' was substituted"));
                }
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning)
                << "Failed to load project embedded presets: " << e.what();
        }
    }

    // B3: Validate presets against system profiles
    try {
        std::set<std::string> modified_gcodes;
        int validated = bundle.validate_presets(
            m_cfg.input_file, m_config, modified_gcodes);

        switch (validated) {
        case VALIDATE_PRESETS_SUCCESS:
            BOOST_LOG_TRIVIAL(info) << "Preset validation passed";
            break;

        case VALIDATE_PRESETS_PRINTER_NOT_FOUND: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom printer preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_PRINTER_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_FILAMENTS_NOT_FOUND: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom filament preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_FILAMENT_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_MODIFIED_GCODES: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom G-code detected in presets (" + details
                + ") — retained as-is (desktop parity)";
            m_stats.issues.push_back(make_warning(-1, "PRESET_MODIFIED_GCODES", msg));
            break;
        }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "Preset validation error: " << e.what();
    }
}

void SliceEngine::apply_printer_preset_config()
{
    if (!m_presets_available || !m_preset_bundle) {
        std::string msg = "System presets not available; cannot verify printer configuration.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRINTER_PRESET_MISSING", msg));
        return;
    }

    // Verify printer-specific parameters have been overridden by the U1
    // preset and are NOT still at FullPrintConfig defaults.
    // FullPrintConfig sets printable_area = [(0,0),(200,0),(200,200),(0,200)]
    // and printable_height = 100.0 as generic placeholders. If these survive
    // the preset merge, the U1 preset did not take effect.
    {
        auto fail = [&](const std::string& detail) {
            std::string msg = "Printer configuration incomplete: " + detail
                + ". The U1 printer preset was not applied correctly. "
                "Verify the resources directory contains Snapmaker U1 machine profiles.";
            BOOST_LOG_TRIVIAL(error) << msg;
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
            m_stats.error_message = msg;
            m_stats.issues.push_back(make_error(-1, "PRINTER_PRESET_NOT_APPLIED", msg));
        };

        // printable_area: default is 4-point 200x200 rect
        auto pa = m_config.option<ConfigOptionPoints>("printable_area");
        if (!pa || pa->values.size() != 4) {
            fail("printable_area missing or wrong format");
        } else {
            bool is_default =
                (pa->values[0].x() == 0.0 && pa->values[0].y() == 0.0) &&
                (pa->values[1].x() == DEFAULT_PLATE_WIDTH &&
                 pa->values[1].y() == 0.0) &&
                (pa->values[2].x() == DEFAULT_PLATE_WIDTH &&
                 pa->values[2].y() == DEFAULT_PLATE_DEPTH) &&
                (pa->values[3].x() == 0.0 &&
                 pa->values[3].y() == DEFAULT_PLATE_DEPTH);
            if (is_default) fail("printable_area is still the default");
        }

        // printable_height: default is DEFAULT_PRINTABLE_HEIGHT
        auto ph = m_config.option<ConfigOptionFloat>("printable_height");
        if (!ph || ph->value == DEFAULT_PRINTABLE_HEIGHT)
            fail("printable_height is still the default");
    }
}

bool SliceEngine::has_inline_filament_config(int ext_idx)
{
    // Check whether the config has per-extruder filament parameters for
    // the given extruder index, even when no named filament preset exists.
    // This handles 3MF files where filament config values are embedded
    // directly in project_settings.config without a preset definition.
    auto is_non_nil = [&](const char* key) -> bool {
        if (!m_config.has(key)) return false;
        auto opt = m_config.option<ConfigOptionFloats>(key, true);
        if (!opt) return false;
        if (static_cast<int>(opt->values.size()) <= ext_idx) return false;
        return !opt->is_nil(ext_idx) && opt->values[ext_idx] > 0;
    };

    // nozzle_temperature or filament_diameter is a strong signal that
    // filament config is present inline.
    if (is_non_nil("nozzle_temperature")) return true;
    if (is_non_nil("filament_diameter")) return true;

    // Fallback: check if filament_type is set (weaker signal, but
    // confirms the extruder has filament assigned).
    if (m_config.has("filament_type")) {
        auto ft = m_config.option<ConfigOptionStrings>("filament_type", true);
        if (ft && ext_idx < static_cast<int>(ft->values.size())
              && !ft->values[ext_idx].empty())
            return true;
    }

    return false;
}

bool SliceEngine::validate_filament_official(bool enforce)
{
    // Skip if system presets are not available (no reference for comparison)
    if (!m_presets_available || !m_preset_bundle)
        return true;

    if (!m_config.has("filament_settings_id"))
        return true;

    auto filament_ids = m_config.option<ConfigOptionStrings>("filament_settings_id", true);
    if (!filament_ids || filament_ids->values.empty())
        return true;

    int num_filaments = static_cast<int>(filament_ids->values.size());
    bool any_error = false;

    // Helper: report an issue at the appropriate severity.
    // When not enforcing, non-official filaments are warnings instead of errors.
    // Truly invalid presets (missing, broken inheritance) remain errors either way.
    auto report = [&](bool is_official_violation, const std::string& code, const std::string& msg) {
        if (!enforce && is_official_violation) {
            BOOST_LOG_TRIVIAL(warning) << msg;
            m_stats.issues.push_back(make_warning(-1, code, msg));
        } else {
            BOOST_LOG_TRIVIAL(error) << msg;
            m_stats.issues.push_back(make_error(-1, code, msg));
            any_error = true;
        }
    };

    // Lambda: check whether a system preset is "official" (Snapmaker or OrcaFilamentLibrary)
    auto is_official_preset = [](const Preset& p) -> bool {
        if (p.vendor && p.vendor->name == PresetBundle::SM_BUNDLE)
            return true;
        if (p.m_from_orca_filament_lib)
            return true;
        return false;
    };

    // Look up a preset name: system presets first, then project embedded.
    // When find_preset's binary search fails (known ordering issue with
    // Snapmaker presets), fall back to linear scan of all loaded presets.
    auto find_in_system = [this](const std::string& name) -> Preset* {
        auto p = m_preset_bundle->filaments.find_preset(name, false);
        if (p && p->name == name) return p;
        // Binary search failed — linear scan fallback
        for (auto& preset : m_preset_bundle->filaments) {
            if (preset.name == name) return &preset;
        }
        return nullptr;
    };

    auto find_in_project = [this](const std::string& name) -> Preset* {
        for (auto pp : m_project_presets) {
            if (pp && pp->name == name && pp->type == Preset::TYPE_FILAMENT)
                return pp;
        }
        return nullptr;
    };

    for (int i = 0; i < num_filaments; ++i) {
        const std::string& name = filament_ids->values[i];

        // Case 1: Direct system preset match
        Preset* sys = find_in_system(name);
        if (sys && is_official_preset(*sys)) {
            continue; // OK — directly matches an official system preset
        }

        // Case 1b: File-based check — preset exists on disk but not
        // found via find_preset (known issue with binary search ordering)
        if (!sys && is_official_filament_file(name)) {
            continue; // OK
        }

        // Case 2: Try project-embedded presets.
        // When sys is non-null but not official (e.g., a project-embedded
        // preset loaded into the bundle via load_project_embedded_presets),
        // use it as the starting point for the inherits-chain walk.
        Preset* current = sys;  // may be a non-official system match
        if (!current) {
            current = find_in_project(name);
        }

        if (!current) {
            // When not enforcing, filament config values may be embedded
            // directly in project_settings.config without a named preset.
            if (!enforce && has_inline_filament_config(i)) {
                std::string msg = "Filament \"" + name
                    + "\" is a custom filament without a preset definition"
                    + " — accepted in allow-custom mode";
                BOOST_LOG_TRIVIAL(warning) << msg;
                m_stats.issues.push_back(make_warning(-1, "FILAMENT_CUSTOM_INLINE", msg));
                continue;
            }

            std::string msg = "Filament \"" + name + "\" is not a recognized preset";
            // FILAMENT_UNKNOWN: preset truly doesn't exist — always an error
            report(/*is_official_violation=*/false, "FILAMENT_UNKNOWN", msg);
            continue;
        }

        // Common: look up a parent by name in system or project presets.
        auto find_ancestor = [&](const std::string& inherits_name) -> Preset* {
            if (Preset* p = find_in_system(inherits_name)) return p;
            return find_in_project(inherits_name);
        };

        // Common walk: follow the inheritance chain, detecting circular refs
        // and unknown ancestors. 'walk' is advanced through the chain; returns
        // false if a structural error is found (circular / unknown ancestor).
        auto walk_chain = [&](Preset*& walk, std::set<std::string>& visited) -> bool {
            while (walk) {
                std::string inherits_name = walk->inherits();
                if (inherits_name.empty()) return true; // root reached
                if (!visited.insert(inherits_name).second) {
                    std::string msg = "Circular inheritance detected in filament \"" + name + "\"";
                    BOOST_LOG_TRIVIAL(error) << msg;
                    m_stats.issues.push_back(make_error(-1, "FILAMENT_CIRCULAR_INHERITS", msg));
                    any_error = true;
                    return false;
                }
                Preset* next = find_ancestor(inherits_name);
                if (!next) {
                    // Report as warning to avoid blocking the slice in non-enforce mode.
                    std::string msg = "Filament \"" + name + "\" inherits from unknown preset \""
                                    + inherits_name + "\"";
                    BOOST_LOG_TRIVIAL(warning) << msg;
                    m_stats.issues.push_back(make_warning(-1, "FILAMENT_UNKNOWN_ANCESTOR", msg));
                    return false;
                }
                walk = next;
            }
            return true; // empty inherits
        };

        // Non-enforce mode: validate structural soundness, accept regardless of ancestry.
        if (!enforce) {
            Preset* walk = current;
            std::set<std::string> visited;
            if (!walk_chain(walk, visited)) {
                // walk_chain already emitted FILAMENT_UNKNOWN_ANCESTOR warning.
                // Cannot auto-recover — skip this filament.
                continue;
            }
            // Custom filament with sound structure — accepted with warning
            std::string msg = "Filament \"" + name + "\" is a custom preset (not official)";
            BOOST_LOG_TRIVIAL(warning) << msg;
            m_stats.issues.push_back(make_warning(-1, "FILAMENT_CUSTOM", msg));
            continue;
        }

        // Enforce mode: must resolve to an official ancestor.
        bool resolved = false;
        std::set<std::string> visited;
        while (current && !resolved) {
            std::string inherits_name = current->inherits();
            if (inherits_name.empty()) {
                std::string msg = "Filament \"" + name
                    + "\" is not derived from any Snapmaker or Generic filament";
                BOOST_LOG_TRIVIAL(error) << msg;
                m_stats.issues.push_back(make_error(-1, "FILAMENT_NO_OFFICIAL_ANCESTOR", msg));
                any_error = true;
                break;
            }

            if (!visited.insert(inherits_name).second) {
                std::string msg = "Circular inheritance detected in filament \"" + name + "\"";
                BOOST_LOG_TRIVIAL(error) << msg;
                m_stats.issues.push_back(make_error(-1, "FILAMENT_CIRCULAR_INHERITS", msg));
                any_error = true;
                break;
            }

            Preset* parent = find_ancestor(inherits_name);
            if (!parent) {
                std::string msg = "Filament \"" + name + "\" inherits from unknown preset \""
                                + inherits_name + "\"";
                BOOST_LOG_TRIVIAL(error) << msg;
                m_stats.issues.push_back(make_error(-1, "FILAMENT_UNKNOWN_ANCESTOR", msg));
                any_error = true;
                break;
            }

            if (is_official_preset(*parent)) {
                substitute_filament_params(filament_ids, i, *parent, name);
                resolved = true;
            } else if (parent->vendor) {
                std::string vendor_name = parent->vendor->name;
                std::string msg = "Filament \"" + name + "\" derives from unsupported vendor \""
                                + vendor_name + "\" via \"" + inherits_name + "\"";
                BOOST_LOG_TRIVIAL(error) << msg;
                m_stats.issues.push_back(make_error(-1, "FILAMENT_UNSUPPORTED_VENDOR", msg));
                any_error = true;
                resolved = true;
            } else if (parent->type == Preset::TYPE_FILAMENT) {
                // Project-embedded filament — continue walking up
                current = parent;
            } else {
                std::string msg = "Filament \"" + name + "\" inherits from non-filament preset \""
                                + inherits_name + "\"";
                BOOST_LOG_TRIVIAL(error) << msg;
                m_stats.issues.push_back(make_error(-1, "FILAMENT_UNKNOWN_ANCESTOR", msg));
                any_error = true;
                resolved = true;
            }
        }
    }

    if (any_error) {
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
    }

    return !any_error;
}

void SliceEngine::substitute_filament_params(ConfigOptionStrings* filament_ids, int ext_idx,
                                              const Preset& official_parent,
                                              const std::string& original_name)
{
    filament_ids->values[ext_idx] = official_parent.name;

    const size_t dst_idx = static_cast<size_t>(ext_idx);

    // Iterate all config keys from the official parent preset.
    // Use ConfigOption::is_nil() and set_at() to handle all types uniformly,
    // including Nullable variants (nozzle_temperature, hot_plate_temp, etc.)
    // that were previously skipped by explicit dynamic_cast branches.
    for (auto it = official_parent.config.cbegin(); it != official_parent.config.cend(); ++it) {
        const auto& key     = it->first;
        const auto& src_opt = it->second;

        auto dst_opt = m_config.option(key, true);  // get or create
        if (!dst_opt) continue;

        // Only vector options can have per-extruder values; scalar options are shared.
        auto dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
        if (!dst_vec) continue;
        if (dst_vec->size() <= dst_idx) continue;

        // Only fill missing values — do not overwrite user-specified settings.
        if (!dst_vec->is_nil(dst_idx)) continue;

        // Copy parent preset's first extruder value into the target extruder slot.
        auto src_vec = dynamic_cast<const ConfigOptionVectorBase*>(src_opt.get());
        if (src_vec && src_vec->size() > 0)
            dst_vec->set_at(src_vec, dst_idx, 0);
    }

    const std::string msg = original_name == official_parent.name
        ? std::string("Filament \"") + original_name
            + "\" config values updated from official preset"
        : std::string("Custom filament \"") + original_name
            + "\" replaced with official preset \""
            + official_parent.name + "\" for cloud safety";
    m_stats.issues.push_back(make_warning(-1, "FILAMENT_SUBSTITUTED", msg));
}

bool SliceEngine::validate_printer_model()
{
    const std::string ALLOWED_PRINTER_MODEL = "Snapmaker U1";

    if (!m_config.has("printer_model")) {
        std::string msg = "Printer model is missing. Only Snapmaker U1 is supported.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRINTER_MODEL_MISSING", msg));
        return false;
    }

    std::string printer_model = m_config.opt_string("printer_model");
    if (printer_model != ALLOWED_PRINTER_MODEL) {
        std::string msg = "Unsupported printer model: \"" + printer_model
                        + "\". Only Snapmaker U1 is supported.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "PRINTER_MODEL_UNSUPPORTED", msg));
        return false;
    }

    return true;
}

// ============================================================================
// Build a full print config by merging system presets (printer + filament)
// underneath the project config from the 3MF.  This mirrors what the desktop
// does via PresetBundle::full_config() / full_fff_config() and ensures that
// filament-level keys (nozzle_temperature, hot_plate_temp, etc.) carry the
// values from the matching system filament preset rather than the raw
// FullPrintConfig defaults.
// ============================================================================

DynamicPrintConfig SliceEngine::build_full_print_config()
{
    DynamicPrintConfig out;
    out.apply(FullPrintConfig::defaults());

    if (m_presets_available && m_preset_bundle) {
        auto& bundle = *m_preset_bundle;

        // Layer 1: System print (process) preset config
        // This matches PresetBundle::full_fff_config() in the desktop —
        // process settings (layer_height, speeds, etc.) must be sourced
        // from the system profile to fill defaults not overridden by
        // project config.
        auto print_id_opt = m_config.option<ConfigOptionString>("print_settings_id");
        if (print_id_opt && !print_id_opt->value.empty()) {
            const Preset* process_preset = bundle.prints.find_preset(
                print_id_opt->value, true);
            if (process_preset)
                out.apply(process_preset->config);
        }

        // Layer 2: System printer config
        auto printer_id_opt = m_config.option<ConfigOptionString>("printer_settings_id");
        if (printer_id_opt && !printer_id_opt->value.empty()) {
            const Preset* printer_preset = bundle.printers.find_preset(printer_id_opt->value, true);
            if (printer_preset)
                out.apply(printer_preset->config);
        }

        // Layer 3: System filament config (per-extruder)
        auto filament_ids = m_config.option<ConfigOptionStrings>("filament_settings_id");
        if (filament_ids && !filament_ids->values.empty()) {
            // Normalize: if model has more filaments than printer extruders,
            // trim to printer extruder count to prevent crashes in wipe tower /
            // ToolOrdering which index flush_volumes_matrix by extruder ID.
            if (out.has("nozzle_diameter")) {
                auto* nd = out.option<ConfigOptionFloats>("nozzle_diameter");
                if (nd && nd->values.size() < filament_ids->values.size()) {
                    const size_t original_count = filament_ids->values.size();

                    // Determine the correct trim target.
                    // Priority 1: variant config nozzle_diameter size (normal case).
                    // Priority 2: fall back to system preset by printer_model + variant.
                    // Priority 3: if target still <= 1, skip trimming entirely.
                    size_t target = nd->values.size();

                    if (target <= 1) {
                        // Try fallback: look up the official system preset
                        std::string printer_model = m_config.opt_string("printer_model");
                        if (!printer_model.empty()) {
                            std::string printer_variant = m_config.opt_string("printer_variant");
                            const Preset* sys_preset =
                                bundle.printers.find_system_preset_by_model_and_variant(
                                    printer_model, printer_variant);
                            if (sys_preset && sys_preset->config.has("nozzle_diameter")) {
                                auto* sys_nd = sys_preset->config.option<ConfigOptionFloats>("nozzle_diameter");
                                if (sys_nd && sys_nd->values.size() > 1)
                                    target = sys_nd->values.size();
                            }
                        }
                    }

                    if (target <= 1) {
                        // Cannot determine a reliable extruder count — skip trim
                        BOOST_LOG_TRIVIAL(warning)
                            << "Cannot determine printer extruder count (nozzle_diameter size="
                            << nd->values.size() << "); keeping " << original_count << " filaments";
                    } else if (target < original_count) {
                        // Trim to target
                        BOOST_LOG_TRIVIAL(warning) << "Trimming filament count from "
                            << original_count << " to " << target
                            << " to match printer extruder count";
                        filament_ids->values.resize(target);
                        // Trim other per-extruder arrays in m_config
                        const char* per_filament_keys[] = {
                            "filament_diameter", "filament_density", "filament_cost",
                            "nozzle_temperature", "nozzle_temperature_initial_layer",
                            "filament_type", "filament_colour", "filament_vendor", nullptr
                        };
                        for (int k = 0; per_filament_keys[k]; ++k) {
                            if (m_config.has(per_filament_keys[k])) {
                                auto* opt = m_config.option(per_filament_keys[k]);
                                if (opt && opt->is_vector()) {
                                    auto* vec = dynamic_cast<ConfigOptionStrings*>(opt);
                                    if (vec && vec->values.size() > target) vec->values.resize(target);
                                    auto* vecf = dynamic_cast<ConfigOptionFloats*>(opt);
                                    if (vecf && vecf->values.size() > target) vecf->values.resize(target);
                                    auto* vecs = dynamic_cast<ConfigOptionFloatsNullable*>(opt);
                                    if (vecs && vecs->values.size() > target) vecs->values.resize(target);
                                }
                            }
                        }

                        // Report to issues and mark as postprocess warning
                        m_stats.issues.push_back(make_warning(-1, "FILAMENT_COUNT_MISMATCH",
                            "Filament count trimmed from "
                            + std::to_string(original_count) + " to "
                            + std::to_string(target)
                            + ": the model references " + std::to_string(original_count)
                            + " filaments but the printer supports only "
                            + std::to_string(target) + " extruders. "
                            + "The excess filaments have been dropped, which may affect "
                            + "multi-color/material output."));
                        m_any_postprocess_warning = true;
                    }
                }
            }

            const size_t num_filaments = filament_ids->values.size();

            // Collect filament config pointers for each extruder.
            std::vector<const DynamicPrintConfig*> filament_configs;
            for (size_t i = 0; i < num_filaments; ++i) {
                const Preset* preset = bundle.filaments.find_preset(filament_ids->values[i], true);
                if (preset)
                    filament_configs.push_back(&preset->config);
            }

            if (!filament_configs.empty()) {
                // Merge per-filament values into `out`, mirroring
                // PresetBundle::full_fff_config() multi-filament logic.
                for (const auto& key : filament_configs.front()->keys()) {
                    if (key == "compatible_prints" || key == "compatible_printers")
                        continue;

                    ConfigOption* dst_opt = out.option(key, false);
                    if (!dst_opt) continue;

                    if (dst_opt->is_scalar()) {
                        const ConfigOption* src = filament_configs.front()->option(key);
                        if (src) dst_opt->set(src);
                    } else {
                        auto dst_vec = static_cast<ConfigOptionVectorBase*>(dst_opt);
                        std::vector<const ConfigOption*> opts(num_filaments, nullptr);
                        for (size_t i = 0; i < num_filaments; ++i)
                            opts[i] = (i < filament_configs.size())
                                          ? filament_configs[i]->option(key)
                                          : nullptr;
                        dst_vec->set(opts);
                    }
                }
            }
        }
    }

    // Layer 4: Project config from 3MF (highest priority)
    out.apply(m_config);

    return out;
}

// ============================================================================
// Stage 3: Validate input
// ============================================================================

bool SliceEngine::validate_input() {
    // Check plate availability
    if (m_plate_data.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No plate data in 3MF, treating as single plate";
        PlateData* pd = new PlateData();
        pd->plate_index = 0;
        m_plate_data.push_back(pd);
    }

    BOOST_LOG_TRIVIAL(info) << "Found " << m_plate_data.size() << " plate(s) in 3MF";

    // Validate requested plate exists
    if (m_cfg.single_plate) {
        // Internal plate_index is 0-based (from 3MF import), CLI plate_id is 1-based
        bool plate_found = false;
        for (const auto& pd : m_plate_data) {
            if (pd->plate_index + 1 == m_cfg.plate_id) {
                plate_found = true;
                break;
            }
        }
        if (!plate_found) {
            BOOST_LOG_TRIVIAL(error) << "Plate " << m_cfg.plate_id << " not found in 3MF file";
            std::cerr << "Available plates: ";
            for (size_t i = 0; i < m_plate_data.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << m_plate_data[i]->plate_index + 1;
            }
            std::cerr << std::endl;
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
// Stage 4: Package output as gcode.3mf
// ============================================================================

void SliceEngine::package_output() {
    BOOST_LOG_TRIVIAL(info) << "Creating gcode.3mf package...";

    std::string printer_model_id;
    std::string nozzle_diameters_str;

    if (m_config.has("printer_model"))
        printer_model_id = m_config.opt_string("printer_model");

    if (m_config.has("nozzle_diameter")) {
        auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt && !nozzle_opt->values.empty()) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2);
            for (size_t i = 0; i < nozzle_opt->values.size(); ++i) {
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

    for (auto& pd : m_plate_data) {
        auto it = m_plate_results.find(pd->plate_index);
        if (it == m_plate_results.end()) continue;

        PlateSliceResult& result = it->second;
        pd->gcode_file = result.gcode_path;
        pd->is_sliced_valid = true;
        pd->printer_model_id = printer_model_id;
        pd->nozzle_diameters = nozzle_diameters_str;

        auto& modes = result.gcode_result.print_statistics.modes;
        const int print_time = static_cast<int>(
            modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
        pd->gcode_prediction = std::to_string(print_time);

        if (result.total_weight != 0.0) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << result.total_weight;
            pd->gcode_weight = ss.str();
        }

        pd->toolpath_outside = result.gcode_result.toolpath_outside;
        pd->timelapse_warning_code = result.gcode_result.timelapse_warning_code;
        pd->is_support_used = result.support_used;
        pd->is_label_object_enabled = result.gcode_result.label_object_enabled;

        pd->parse_filament_info(&result.gcode_result);

        for (auto& info : pd->slice_filaments_info) {
            size_t idx = static_cast<size_t>(info.id);
            if (filament_types && idx < filament_types->values.size())
                info.type = filament_types->values[idx];
            if (filament_colors && idx < filament_colors->values.size())
                info.color = filament_colors->values[idx];
            if (filament_ids && idx < filament_ids->values.size())
                info.filament_id = filament_ids->values[idx];
        }

        // Rebuild objects_and_instances using model.objects array indices
        std::set<int> plate_plate_instance_ids;
        for (const auto& entry : pd->obj_inst_map)
            plate_plate_instance_ids.insert(entry.second.second);

        pd->objects_and_instances.clear();
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx) {
            const ModelObject* obj = m_model.objects[obj_idx];
            for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx) {
                const ModelInstance* inst = obj->instances[inst_idx];
                if (plate_plate_instance_ids.count(static_cast<int>(inst->loaded_id)))
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

    std::vector<ThumbnailData*> thumbnail_data;
    std::vector<ThumbnailData*> no_light_thumbnail_data;
    std::vector<ThumbnailData*> top_thumbnail_data;
    std::vector<ThumbnailData*> pick_thumbnail_data;
    std::vector<ThumbnailData*> calibration_thumbnail_data;

    // Populate thumbnail vectors from decoded plate data so they are
    // embedded into the output .3mf package (Metadata/plate_*.png).
    for (const auto& pd : m_plate_data) {
        thumbnail_data.push_back(
            pd->plate_thumbnail.is_valid() ? &pd->plate_thumbnail : nullptr);
        no_light_thumbnail_data.push_back(nullptr);
        top_thumbnail_data.push_back(nullptr);
        pick_thumbnail_data.push_back(nullptr);
        calibration_thumbnail_data.push_back(nullptr);
    }

    std::vector<PlateBBoxData*> id_bboxes;
    std::vector<std::unique_ptr<PlateBBoxData>> id_bboxes_owned;
    id_bboxes_owned.reserve(m_plate_data.size());
    for (size_t i = 0; i < m_plate_data.size(); ++i) {
        id_bboxes_owned.push_back(std::make_unique<PlateBBoxData>());
        id_bboxes.push_back(id_bboxes_owned.back().get());
    }

    params.thumbnail_data = thumbnail_data;
    params.no_light_thumbnail_data = no_light_thumbnail_data;
    params.top_thumbnail_data = top_thumbnail_data;
    params.pick_thumbnail_data = pick_thumbnail_data;
    params.calibration_thumbnail_data = calibration_thumbnail_data;
    params.id_bboxes = id_bboxes;
    params.project = nullptr;
    params.profile = nullptr;

    try {
        ScopedLogLevel quiet_pkg(boost::log::trivial::warning);
        bool success = store_bbs_3mf(params);
        if (!success) {
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
    catch (const std::exception& e) {
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

void SliceEngine::report_error(int plate_id, int exit_code, const std::string& code,
                               const std::string& message, bool set_main_message) {
    m_stats_builder->report_error(plate_id, exit_code, code, message, set_main_message);
}

void SliceEngine::set_error_type(int code) {
    m_stats_builder->set_error_type(code);
}

int SliceEngine::exit_code() const {
    return m_stats_builder->exit_code();
}

// ============================================================================
// Stage 5: Build statistics for JSON output
// ============================================================================

void SliceEngine::build_statistics() {
    for (const auto& [plate_id, result] : m_plate_results) {
        SliceOutputStats::PlateStats plate_stats;
        plate_stats.plate_id = plate_id;

        bool plate_has_error = false;
        bool plate_has_warning = false;
        for (const auto& issue : result.issues) {
            if (issue.level == "error") plate_has_error = true;
            if (issue.level == "warning") plate_has_warning = true;
            m_stats.issues.push_back(issue);
        }

        plate_stats.success = !plate_has_error;
        if (plate_has_error) {
            m_any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
        }
        if (plate_has_warning || result.has_postprocess_warning)
            m_any_postprocess_warning = true;

        plate_stats.issues = result.issues;

        if (plate_stats.success) {
            plate_stats.gcode_file = m_output_path;

            auto& modes = result.gcode_result.print_statistics.modes;
            auto& normal_mode = modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
            plate_stats.total_time = normal_mode.time;
            plate_stats.prepare_time = normal_mode.prepare_time;
            plate_stats.print_time = normal_mode.time - normal_mode.prepare_time;
            if (!std::isfinite(plate_stats.print_time)) {
                BOOST_LOG_TRIVIAL(warning)
                    << "Plate " << plate_id << " print time is non-finite ("
                    << normal_mode.time << " - " << normal_mode.prepare_time
                    << "), falling back to 0";
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

            for (const auto& [extruder_id, volume] : result.filament_volumes) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
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
            for (const auto& [extruder_id, volume] : ps.support_volumes_per_extruder) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_support_m += (volume / cross_section) * 0.001;
                    total_support_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.wipe_tower_volumes_per_extruder) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_wipe_tower_m += (volume / cross_section) * 0.001;
                    total_wipe_tower_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.flush_per_filament) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_flush_m += (volume / cross_section) * 0.001;
                    total_flush_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            plate_stats.model_filament_m = plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
            plate_stats.model_filament_g = plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;

            if (m_config.has("nozzle_diameter")) {
                auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
                if (nozzle_opt)
                    plate_stats.nozzle_diameters = nozzle_opt->values;
            }

            plate_stats.plate_count = static_cast<int>(m_plate_data.size());

            const ConfigOptionStrings* ftypes =
                m_config.has("filament_type") ? m_config.option<ConfigOptionStrings>("filament_type") : nullptr;
            const ConfigOptionStrings* fcolors =
                m_config.has("filament_colour") ? m_config.option<ConfigOptionStrings>("filament_colour") : nullptr;

            for (const auto& [extruder_id, used_g] : plate_stats.filament_used_g) {
                SliceOutputStats::FilamentDetail detail;
                detail.id = extruder_id;
                detail.used_g = used_g;
                detail.used_m = plate_stats.filament_used_m.count(extruder_id)
                                     ? plate_stats.filament_used_m.at(extruder_id)
                                     : 0.0;

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
            for (const auto pd : m_plate_data) {
                if (pd->plate_index == plate_id && !pd->thumbnail_file.empty()) {
                    plate_stats.model_thumbnail = "Metadata/plate_" + std::to_string(plate_id + 1) + ".png";
                    break;
                }
            }
        } else {
            plate_stats.plate_count = static_cast<int>(m_plate_data.size());
        }

        m_stats.plates.push_back(plate_stats);
    }

    // Add placeholder plates for plates with issues but not in plate_results
    std::set<int> plates_with_results;
    for (const auto& p : m_stats.plates)
        plates_with_results.insert(p.plate_id);

    for (const auto& issue : m_stats.issues) {
        if (issue.plate_id >= 0 && plates_with_results.find(issue.plate_id) == plates_with_results.end()) {
            bool found = false;
            for (const auto& p : m_stats.plates) {
                if (p.plate_id == issue.plate_id) { found = true; break; }
            }
            if (!found) {
                SliceOutputStats::PlateStats failed_plate;
                failed_plate.plate_id = issue.plate_id;
                failed_plate.success = false;
                failed_plate.plate_count = static_cast<int>(m_plate_data.size());
                failed_plate.issues.push_back(issue);
                plates_with_results.insert(issue.plate_id);
                m_stats.plates.push_back(failed_plate);
            } else {
                for (auto& p : m_stats.plates) {
                    if (p.plate_id == issue.plate_id) {
                        p.issues.push_back(issue);
                        break;
                    }
                }
            }
        }
    }

    // Sort plates by plate_id
    std::sort(m_stats.plates.begin(), m_stats.plates.end(),
        [](const SliceOutputStats::PlateStats& a, const SliceOutputStats::PlateStats& b) {
            return a.plate_id < b.plate_id;
        });

    // Determine global success
    m_stats.success = !m_stats.plates.empty();
    for (const auto& p : m_stats.plates) {
        if (!p.success) {
            m_stats.success = false;
            break;
        }
    }
    if (!m_stats.success && m_stats.error_message.empty()) {
        if (m_stats.plates.empty())
            m_stats.error_message = "No plates completed successfully";
        else {
            int failed_count = 0;
            for (const auto& p : m_stats.plates)
                if (!p.success) ++failed_count;
            if (failed_count == static_cast<int>(m_stats.plates.size()))
                m_stats.error_message = "All plates failed with errors";
            else
                m_stats.error_message = "Some plates failed with errors";
        }
    }
}
