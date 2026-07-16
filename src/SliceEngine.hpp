#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Semver.hpp"

#include "Types.hpp"

struct EngineConfig
{
    std::string input_file;
    std::string output_base; // -o value, empty = auto-derive from input name
    int plate_id = 0; // 0 = all plates
    OutputFormat format = OutputFormat::GCODE_3MF;
    bool single_plate = false;
    std::string temp_dir; // temp directory for intermediate gcode files
    int timeout_seconds = 0; // 0 = no timeout; cloud service sets based on file size
    int max_size_mb = 200; // 0 = no limit; max input file size in megabytes
    std::string cancel_file; // watchdog file path for external cancellation
    std::string log_path; // log file path (empty = auto-derive from output)
    std::string json_output_path; // -j/--json: path for JSON output file (empty = auto-derive from output)
    bool skip_preset_substitution = false; // --skip-preset-substitution
};

// Intermediate result for a single plate during the pipeline
struct PlateSliceResult
{
    std::string gcode_path;
    Slic3r::GCodeProcessorResult gcode_result;
    double total_weight = 0.0;
    bool support_used = false;
    bool has_postprocess_warning = false;
    double total_used_filament = 0.0;
    double total_cost = 0.0;
    std::map<size_t, double> filament_volumes; // per extruder
    std::vector<Issue> issues; // collected issues for this plate
};

class SliceEngine
{
public:
    SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files);
    ~SliceEngine();

    // Run the full pipeline. Returns true if at least one plate produced output.
    bool run();

    // Results after run()
    const SliceOutputStats& stats() const
    {
        return m_stats;
    }
    const std::string& output_path() const
    {
        return m_output_path;
    }

    // Accessors for exit-code determination
    bool any_error() const
    {
        return m_any_error;
    }
    bool any_postprocess_warning() const
    {
        return m_any_postprocess_warning;
    }

    // Returns the most specific exit code based on what failed.
    // Precedence: validation > slicing > export > load > postprocess_warning > ok.
    int exit_code() const;
    void set_error_type(int code);

private:
    // --- Pipeline stages (in call order) ---
    bool load_3mf();
    bool validate_printer_model();
    void validate_config();
    void load_system_presets();
    bool validate_presets();
    bool apply_printer_official_preset();
    bool apply_filament_official_preset();
    void substitute_filament_params(Slic3r::ConfigOptionStrings* filament_ids, int ext_idx,
                                    const Slic3r::Preset& official_parent, const std::string& original_name);
    void apply_process_official_preset();
    bool validate_input();
    void ensure_models_on_bed();

    /**
     * @brief Load and decode plate thumbnails from disk for the current plate set.
     *
     * The input .3mf load path (_BBS_3MF_Importer::_load_model_from_file in
     * libslic3r) only sets PlateData::thumbnail_file (an absolute disk path
     * written under the backup_path temp directory) and never populates
     * plate_thumbnail.pixels. The legacy `if (pixels.empty()) continue` guard
     * was therefore a silent no-op for every plate, leaving the export_gcode
     * thumbnail callback with no source and producing gcode without
     * `; thumbnail begin` markers.
     *
     * This function reads PNG bytes from PlateData::thumbnail_file and decodes
     * them via Slic3r::png::decode_colored_png.
     *
     * Trust-boundary validation (thumbnail_file originates from .3mf XML,
     * an untrusted source in multi-tenant upload scenarios):
     *   1. Path non-empty and ends in ".png"
     *   2. Path resides under the system temp directory (path-injection guard;
     *      engine cannot read libslic3r's internal backup_path)
     *   3. boost::filesystem::exists with error_code (no exceptions)
     *   4. file_size within [1, MAX_PNG_SIZE]
     *   5. gcount() == requested on read (replaces fragile !ifs / EOF check)
     *   6. PNG magic bytes (\x89PNG\r\n\x1a\n) verified
     *   7. decode_colored_png succeeds
     *
     * On any error, the plate is silently skipped (continue) and
     * plate_thumbnail is left in its prior empty state -- matching the .3mf
     * output fallback behavior in bbs_3mf.cpp:5951.
     *
     * Called from run() after load_3mf and before process_plate
     * (SliceEngine.cpp:212). At that point the backup_path temp directory
     * still exists.
     *
     * @note Does NOT throw. All failures route through early-continue.
     * @note Does NOT handle the gcode.3mf load path (path 1 in libslic3r) --
     *       the engine never invokes load_gcode_3mf_from_stream, so
     *       plate_thumbnail.pixels is always empty here.
     */
    void decode_plate_thumbnails();
    void process_plate(int plate_id);
    void package_output();
    void build_statistics();

    // --- Per-plate sub-stages (in call order) ---
    bool filter_instances(int plate_id, std::set<int>& identify_ids);
    Slic3r::Vec3d setup_print_origin(int plate_id, double plate_width, double plate_depth);
    bool run_build_volume_check(int plate_id, const std::set<int>& identify_ids, const Slic3r::Vec3d& origin);
    bool apply_model(int plate_id, Slic3r::Print& print, const Slic3r::Vec3d& origin);
    bool run_validation(int plate_id, Slic3r::Print& print);
    bool run_slicing(int plate_id, Slic3r::Print& print);
    bool export_gcode(int plate_id, Slic3r::Print& print, PlateSliceResult& result);
    void run_postprocessing(int plate_id, PlateSliceResult& result);

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
    int m_error_type = EXIT_OK; // most severe exit code encountered

    // Loaded data
    Slic3r::Model m_model;
    Slic3r::DynamicPrintConfig m_config;
    Slic3r::ConfigSubstitutionContext m_config_substitutions{Slic3r::ForwardCompatibilitySubstitutionRule::Enable};
    // Raw owning pointers populated by Model::read_from_file().
    // Owned and cleaned up by ~SliceEngine() via release_PlateData_list().
    // Cannot use unique_ptr due to libslic3r API compatibility (PlateDataPtrs is
    // a shared type across the library boundary).
    Slic3r::PlateDataPtrs m_plate_data;
    std::vector<Slic3r::Preset*> m_project_presets;
    bool m_is_bbl_3mf = false;
    Slic3r::Semver m_file_version;

    // Preset validation (requires system profiles at resources_dir/profiles/)
    std::unique_ptr<Slic3r::PresetBundle> m_preset_bundle;
    bool m_presets_available = false;
};
