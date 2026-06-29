#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Semver.hpp"

#include <tbb/global_control.h>

#include "Types.hpp"
#include "EngineContext.hpp"

// Forward declarations for sub-components (extracted from SliceEngine)
class PresetManager;
class PlateProcessor;
class StatisticsBuilder;

// Forward declarations for types used by pointer/reference only
namespace Slic3r {
    class Print;
    class Preset;
    struct PlateData;
}
using PlateDataPtrs = std::vector<Slic3r::PlateData*>;

/**
 * @brief Engine configuration from CLI / C API.
 */
struct EngineConfig {
    std::string input_file;
    std::string output_base;       // -o value, empty = auto-derive from input name
    int plate_id = 0;              // 0 = all plates
    OutputFormat format = OutputFormat::GCODE_3MF;
    bool single_plate = false;
    std::string temp_dir;          // temp directory for intermediate gcode files
    int timeout_seconds = 0;       // 0 = no timeout; cloud service sets based on file size
    int max_size_mb = 200;         // 0 = no limit; max input file size in megabytes
    std::string cancel_file;       // watchdog file path for external cancellation
    bool substitute_printer  = true;   // whether to substitute printer preset with official parent
    bool substitute_filaments = true;  // whether to substitute filament presets with official parent
    int  thread_count = 0;            // 0 = use all cores; N = limit TBB to N threads
};

/**
 * @brief Intermediate result for a single plate during the pipeline.
 */
struct PlateSliceResult {
    std::string gcode_path;
    Slic3r::GCodeProcessorResult gcode_result;
    double total_weight = 0.0;
    bool support_used = false;
    bool has_postprocess_warning = false;
    double total_used_filament = 0.0;
    double total_cost = 0.0;
    std::map<size_t, double> filament_volumes;  // per extruder
    std::vector<Issue> issues;                   // collected issues for this plate
};

// ============================================================================
// SliceEngine — headless cloud slicing pipeline
// ============================================================================
//
// Pipeline stages (delegated to sub-components):
//   PresetManager:    load_system_presets → validate_presets →
//                     validate_printer_model → apply_printer_official_preset →
//                     validate_filament_official → build_full_print_config
//   PlateProcessor:   per-plate full pipeline (filter → validate → slice → export →
//                     postprocess), including Z-baking and build-volume checks
//   StatisticsBuilder: package_output → build_statistics
//
// Config flow (four layers, applied in order):
//   1. FullPrintConfig::defaults()  — system defaults
//   2. System process preset  — from print_settings_id (fills unspecified)
//   3. System printer preset  — Snapmaker U1, loaded by nozzle diameter
//   4. System filament preset — per-extruder, from filament_settings_id
//   5. m_config               — 3MF project config (highest priority)
//
class SliceEngine {
public:
    SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files);
    ~SliceEngine();

    /** Run the full pipeline. Returns true if at least one plate produced output. */
    bool run();

    // Results after run()
    const SliceOutputStats& stats() const { return m_stats; }
    const std::string& output_path() const { return m_output_path; }

    /** Returns the most specific exit code based on what failed. */
    int exit_code() const;

    /** Update error type if new code is more severe. */
    void set_error_type(int code);

    /** Record an error issue and update exit-code tracking. */
    void report_error(int plate_id, int exit_code, const std::string& code,
                      const std::string& message, bool set_main_message = false);

private:
    // --- Pipeline stages (delegated to sub-components) ---
    bool load_3mf();
    void decode_plate_thumbnails();
    void sanitize_config();
    void validate_config();
    bool validate_input();


    // --- State ---
    EngineConfig m_cfg;
    std::string m_output_path;
    SliceOutputStats m_stats;
    std::chrono::steady_clock::time_point m_timeout_deadline;
    bool m_has_timeout = false;
    std::vector<std::string>& m_temp_files;
    std::map<int, PlateSliceResult> m_plate_results;
    bool m_any_error = false;
    bool m_any_postprocess_warning = false;
    int m_error_type = EXIT_OK;  // most severe exit code encountered

    // TBB thread count control (kept alive for entire slicing duration)
    std::unique_ptr<tbb::global_control> m_tbb_control;

    // Loaded data
    Slic3r::Model m_model;
    Slic3r::DynamicPrintConfig m_config;
    PlateDataPtrs m_plate_data;
    std::vector<Slic3r::Preset*> m_project_presets;

    Slic3r::ConfigSubstitutionContext m_config_substitutions{
        Slic3r::ForwardCompatibilitySubstitutionRule::Enable};

    std::vector<BakedInstanceZ> m_baked_instance_z;

    // Shared context (references to all mutable state above)
    std::unique_ptr<EngineContext> m_ctx;

    // Extracted sub-components
    std::unique_ptr<PresetManager>    m_presets;
    std::unique_ptr<PlateProcessor>   m_plate_proc;
    std::unique_ptr<StatisticsBuilder> m_stats_builder;

    // Plate dimensions (FullPrintConfig defaults used for validation)
    static constexpr double DEFAULT_PLATE_WIDTH  = 200.0;
    static constexpr double DEFAULT_PLATE_DEPTH  = 200.0;
    static constexpr double DEFAULT_PRINTABLE_HEIGHT = 100.0;
};
