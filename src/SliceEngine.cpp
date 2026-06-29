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


using namespace Slic3r;

namespace {

// 1/5, same as GUI's LOGICAL_PART_PLATE_GAP

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
            nullptr,
            nullptr,
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

