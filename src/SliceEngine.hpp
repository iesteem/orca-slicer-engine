#pragma once

#include <chrono>
#include <map>
#include <set>
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
    std::vector<std::string> filament_colours; // colours actually used (matches G-code header)
    std::vector<std::string> filament_types;   // types actually used (matches G-code header)
    std::vector<double> nozzle_diameters;      // nozzle diameters actually used (matches what slicer used)
    std::vector<double> filament_diameters;    // filament diameters actually used (matches G-code generation)
    std::vector<double> filament_densities;    // filament densities actually used (matches G-code generation)
    std::vector<Issue> issues; // collected issues for this plate
};

class SliceEngine
{
public:
    SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files);
    ~SliceEngine();

    // Run the full pipeline. Returns true if at least one plate produced output.
    bool run();

    // Apply official Snapmaker presets: bundle check, strip user content, then
    // printer/filament/process substitution. Returns false on any fatal preset
    // failure (caller should abort). Skipped entirely when
    // m_cfg.skip_preset_substitution is set.
    bool apply_preset_substitution();

    // One-shot normalization of m_config, run once per pipeline (in every entry
    // point) after the preset-substitution stage — including when substitution
    // was skipped, because both fixes guard against libslic3r SEGVs and are
    // independent of the substitution policy:
    //  (a) backfill PrintConfig keys the 3MF / preset chain left undefined;
    //  (b) repair flush_volumes_matrix / flush_volumes_vector dimensions
    //      (normalize_flush_volumes_matrix).
    // Both are global (plate-agnostic) and idempotent, so running them once on
    // m_config — instead of per-plate inside prepare_merged_config_for_plate —
    // also lets earlier readers (setup_extruder_params, per-plate checks) see
    // the normalized config.
    void normalize_loaded_config();

    // Run only the load + preset-substitution prefix of the pipeline (load_3mf,
    // validate_printer_model, collect_config_warnings, load_system_presets,
    // apply_preset_substitution), then stop — no geometry
    // checks, no slicing, no export. Intended for config-only inspection / cloud
    // preflight: after it returns, m_config holds the substitution result and is
    // readable via config(). Returns false on any failure in the prefix (same
    // per-step error handling as run()). The call sequence MUST stay in sync
    // with the prefix of run(); see SliceEngine.cpp.
    bool run_preset_substitution_only();

    // Run the preset-substitution prefix AND the geometry-half preprocessing
    // prefix of run() (validate_input, run_geometry_checks, bake_instance_z_into_mesh,
    // assign_arrange_order, setup_extruder_params), then stop — no slicing, no
    // export. Intended for integration testing the geometry preprocessing
    // stages: after it returns, m_model holds the post-preprocessing geometry
    // (Z baked into mesh, arrange_order assigned) and is readable via model().
    // Returns false on any failure in the prefix (same per-step error handling
    // as run()). The call sequence MUST stay in sync with run(); see
    // SliceEngine.cpp.
    bool run_geometry_preprocess_only();

    // Read-only access to the merged config (final after the
    // preset-substitution stage plus normalize_loaded_config; run() leaves it
    // unchanged past that point).
    const Slic3r::DynamicPrintConfig& config() const
    {
        return m_config;
    }

    // Read-only access to the loaded model. Geometry-half stages mutate it in
    // place: bake_instance_z_into_mesh moves instance Z into mesh vertices,
    // assign_arrange_order stamps every instance. Final after
    // run_geometry_preprocess_only() / run(). Exposed for integration tests that
    // inspect the resulting model state.
    const Slic3r::Model& model() const
    {
        return m_model;
    }

    // Name of the official process preset that apply_process_official_preset
    // most recently resolved and applied. Empty if none was found/applied.
    // Unlike filament (which writes the resolved name back to
    // filament_settings_id), the process path keeps the name in a local, so
    // expose it here for diagnostics / testing.
    const std::string& last_process_preset_name() const
    {
        return m_last_process_preset_name;
    }

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

    // Load and validate the input .3mf file. Populates m_model, m_config, and
    // m_plate_data on success. Returns false if load or validation fails.
    bool load_3mf();

    // load_3mf sub-stages (private implementation details)

    // Validate the input file path before invoking libslic3r: extension must
    // be .3mf (case-insensitive) and size must respect --max-size (0 = no limit).
    // Returns false if rejected; record_load_error() has already been called.
    bool validate_input_file();

    // Invoke Model::read_from_file with the full load strategy and classify
    // failures. On success m_model is populated. On failure (exception or zero
    // objects) partial outputs are released and an issue is recorded.
    // Returns false on failure.
    // Note: empty-file detection currently relies on a substring match
    // ("empty") in the libslic3r exception message — fragile, see FIXME in cpp.
    bool read_3mf_model();

    // Record a load-stage failure: log, set error flag, set exit code,
    // populate m_stats.error_message and issues. Does NOT throw and does NOT
    // return; the caller decides whether to propagate via return false.
    void record_load_error(const std::string& code, const std::string& msg);

    // Check that the printer model in the config matches a supported Snapmaker
    // model. Returns false and sets an error if the model is unknown.
    bool validate_printer_model();

    // Validate config values (layer_height, nozzle_diameter, etc.) and collect
    // config substitution / unrecognized key warnings into m_stats.issues.
    void collect_config_warnings();

    // Load system printer, filament, and process presets from the resources
    // directory into m_preset_bundle. Must be called before preset substitution.
    void load_system_presets();

    // Unified "is this an official Snapmaker preset" check for the printer,
    // filament, and process substitution paths. Official = vendor is the
    // Snapmaker bundle. Project-embedded presets (no vendor / other vendor)
    // and other vendors' system presets are NOT official — they may be
    // waypoints on an inheritance chain but must never be applied wholesale.
    static bool is_official_preset(const Slic3r::Preset& p);

    // Strip all user-supplied content from m_config: G-code keys (machine +
    // process + filament level), user-authored text (printer_notes /
    // filament_notes), the post_process shell-command list, and external
    // file-loading references (load_custom_gcodes / load_slicedata /
    // load_settings / load_assemble_list).
    // Silent for G-code and post_process (apply_*_official_preset reports the
    // replacement separately as PRINTER/FILAMENT/PROCESS_SUBSTITUTED). Emits
    // one USER_CONTENT_CLEARED tip only when notes or external file references
    // are cleared — those have no official counterpart. Never blocks.
    void strip_user_content();

    // Overwrite every config key in m_config with the official Snapmaker printer
    // preset matching the current nozzle diameter. Returns false on fatal
    // failure (caller should abort).
    bool apply_printer_official_preset();

    // Verify the official printer preset took effect after overwrite_all_keys_from.
    // Checks printable_area (4-point non-default rectangle) and printable_height
    // (non-default). On failure, emits PRINTER_PRESET_NOT_APPLIED error and
    // returns false; caller propagates as a fatal abort.
    bool verify_printer_geometry();

    // Resolve each extruder's filament to an official Snapmaker preset using the
    // two-pass strategy (same vendor → Generic fallback). Returns false if any
    // extruder's filament could not be resolved and rollback also failed.
    bool apply_filament_official_preset();

    // Resolve a single extruder's filament to an official Snapmaker preset.
    // On success, records the outcome in rolled_back or substituted. On failure
    // (no official preset resolvable AND rollback failed), pushes an error
    // issue directly and returns false. See apply_filament_official_preset.cpp
    // for the case-by-case decision tree.
    using FilamentGrouping = std::map<std::pair<std::string, std::string>, std::vector<int>>;

    bool resolve_filament(int i, Slic3r::ConfigOptionStrings* filament_ids,
                          FilamentGrouping& rolled_back, FilamentGrouping& substituted);

    // Emit deduplicated FILAMENT_ROLLED_BACK / FILAMENT_SUBSTITUTED warnings
    // from the grouping maps built during resolve_filament. Message shape
    // follows the unified substitution policy: orig==target → "verified
    // against" (cloud hardening), orig!=target → "from X to Y" (real swap).
    void emit_filament_warnings(const FilamentGrouping& rolled_back, const FilamentGrouping& substituted);

    // Overwrite process config with the official Snapmaker preset, preserving
    // user overrides parsed by parse_process_user_overrides. Emits
    // PROCESS_SUBSTITUTED or PROCESS_VERIFIED_AGAINST warning. Returns false
    // (fatal, EXIT_PREPROCESS_ERROR + PROCESS_PRESET_NOT_OFFICIAL) when no
    // official system ancestor can be found for the user's process preset —
    // the product requires process substitution to succeed, so falling back
    // to raw 3MF values is not acceptable.
    bool apply_process_official_preset();

    // Look up an official system process preset for the given preset name.
    // Tries a direct system match first (Case 1), then walks the inheritance
    // chain through project-embedded presets to find a system ancestor
    // (Case 2). Returns nullptr if neither path resolves. Circular
    // inheritance is detected and logged, then treated as no ancestor.
    // out_inherits_name (optional): when lookup fails, receives the first
    // non-empty inherited preset name seen on the chain — callers use it to
    // tell the user which non-official preset the user's preset traces to.
    const Slic3r::Preset* find_official_process_preset(const std::string& preset_name,
                                                       std::string* out_inherits_name = nullptr) const;

    // Parse different_settings_to_system[0] — the ;-separated list of process
    // keys the user explicitly changed from the system defaults. Returns an
    // empty set if the field is absent or empty. Used by apply_process_official_preset
    // to honour user overrides when applying the official preset.
    std::set<std::string> parse_process_user_overrides();

    // When brim_type=auto_brim, set brim_width=0 so the fallback path (when
    // the algorithm decides no brim is needed) doesn't generate unwanted brim.
    // The algorithm still sets its own computed width when it determines brim
    // IS needed. Matches desktop Snapmaker behaviour.
    void apply_auto_brim_fallback();

    // Validate that the loaded config and model are complete and consistent.
    // Checks plate data integrity and ensures required config keys are present.
    bool validate_input();

    // Relocate each object's Z information from instance space into mesh-vertex
    // space so it survives libslic3r's Print::apply (which discards instance Z).
    // Also applies the desktop "intentional sinking" rule (single-part
    // straddling preserved, fully-buried raised). See SliceEngine.cpp for the
    // full rationale and the per-instance rotation assumption.
    void bake_instance_z_into_mesh();

    // Assign sequential arrange_order values to all model instances. Run-once
    // (global, plate-agnostic) -- called from run() before the per-plate loop.
    void assign_arrange_order();

    // Count filaments from config and populate Model::extruderParamsMap
    // (per-extruder material / bed / end temperatures). Config-only, no Print
    // dependency. Run-once from run() before the per-plate loop.
    void setup_extruder_params();

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
     *   2. boost::filesystem::canonical succeeds (rejects non-existent
     *      and bogus paths, resolves symlinks and ".." components)
     *   3. Canonical path starts with a known Linux system temp prefix
     *      ({/tmp/, /var/tmp/, /private/tmp/}) — NOT temp_directory_path(),
     *      because TMPDIR may differ from libslic3r's internal backup_path.
     *      Linux-only: production engine runs on Linux (cloud server);
     *      Windows dev builds will skip all thumbnails (prefixes don't
     *      match %TEMP%). Add Windows prefixes here if Windows engine
     *      deployment is introduced.
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
     * @note Does NOT throw. All allocation failures (std::bad_alloc from
     *       PNG buffer allocation) and all validation failures route through
     *       early-return, leaving prior successfully-decoded plates intact.
     * @note Does NOT handle the gcode.3mf load path (path 1 in libslic3r) --
     *       the engine never invokes load_gcode_3mf_from_stream, so
     *       plate_thumbnail.pixels is always empty here.
     */
    void decode_plate_thumbnails();

    // Run the full slicing pipeline for a single plate: filter instances, check
    // build volume, slice, export G-code, and post-process. Results are stored
    // in m_plate_results[plate_id].
    void process_plate(int plate_id);

    // Package all successfully sliced plate results (G-code + metadata) into a
    // single .3mf output file. Only called when at least one plate produced output.
    void package_output();

    // Push slice results back into PlateData: gcode path, prediction, weight,
    // support/toolpath flags, filament info (overlaid with config-sourced
    // type/colour/id), and the rebuilt objects_and_instances list. One pass
    // over m_plate_data matching entries to m_plate_results.
    void populate_plate_data_for_export(const std::string& printer_model_id);

    // Build SliceOutputStats from m_plate_results: per-plate issue aggregation,
    // filament usage, success flags, and derived error message. Called once
    // after all plates have been processed.
    void build_statistics();

    // Assemble PlateStats from a single plate's slice result: issue aggregation,
    // success/error flags, time/filament/cost/support fields, per-extruder
    // filament usage, model-vs-support breakdown, nozzle list, filament
    // details, and thumbnail path lookup. Called once per plate.
    void assemble_plate_stats(int plate_id, const PlateSliceResult& result,
                              SliceOutputStats::PlateStats& plate_stats);

    // Add placeholder PlateStats for plates that have issues recorded against
    // them but no entry in m_plate_results (e.g. validation failures that
    // aborted before slicing). Also merges orphan issues into existing plates.
    void patch_orphan_plate_issues();

    // Sort plates and issues by severity/plate_id, compute the global success
    // flag, and derive m_stats.error_message from whichever failure pattern
    // occurred. Called after all per-plate stats are assembled.
    void finalise_statistics();

    // --- Per-plate sub-stages (in call order) ---

    // Collect the loaded_id of every model instance that belongs to the given
    // plate (pure query; does not modify the model).
    std::set<int> collect_plate_instance_ids(int plate_id) const;

    // Mark every model instance printable according to whether its loaded_id is
    // in plate_ids (on-plate -> printable + Inside, off-plate -> not printable +
    // Fully_Outside). Returns false if no instance is on the plate (empty plate).
    bool mark_printable_instances(const std::set<int>& plate_ids);

    // Compute the global coordinate origin for a plate in the grid layout.
    // Plate dimensions are derived internally from m_config "printable_area";
    // returns Zero if printable_area is absent/empty (the build-volume check
    // short-circuits on the same condition, so the origin is unused then).
    Slic3r::Vec3d setup_print_origin(int plate_id);

    // Check all instances on this plate against the build volume: flag objects
    // outside/partly outside the printable area, and check spiral-lift
    // proximity to bed edges. Returns false if any object is partly outside
    // (fatal error).
    bool run_build_volume_check(int plate_id, const std::set<int>& identify_ids, const Slic3r::Vec3d& origin);

    // Subdivide a partly-outside instance's violation into directional error
    // codes (TOO_HIGH / OUTSIDE_XY) using its plate-local world
    // bounding box. `bbox` must already be in plate-local coordinates (plate
    // origin subtracted). One object may yield multiple issues.
    void push_build_volume_issues(int plate_id, const std::string& object_name,
                                  const Slic3r::BoundingBoxf3& bbox, const Slic3r::BuildVolume& build_volume);

    // Guard: returns true while the global slicing deadline has not expired
    // (caller may proceed with heavy slicing). Returns false once it has --
    // logging SLICING_TIMEOUT, pushing the issue, and setting the exit code
    // before returning. Used as a precondition check, hence the question-style
    // name and false-on-expiry convention shared with the other process_plate
    // sub-steps.
    bool within_slicing_deadline(int plate_id);

    // Create a Print object for the plate: set status callback and BBL flag.
    // Returns a configured Print ready for model application.
    void init_print(Slic3r::Print& print);

    // SpiralLiftNearBoundary check (desktop 3DScene.cpp parity): when z_hop
    // uses spiral/auto mode and bed is rectangular, flag any object whose
    // bounding box sits within SPIRAL_LIFT_SAFETY_MARGIN of the bed edge.
    // Returns true if any serious warning was emitted. Deduplicates by
    // object name so multi-instance objects emit one warning.
    void check_spiral_lift_near_boundary(int plate_id, const Slic3r::BuildVolume& build_volume,
                                         const std::set<int>& identify_ids);

    // Prepare this plate's Print: stamp plate index + grid-layout origin, build
    // the per-plate merged config, and apply the model objects. Returns false if
    // no printable objects result.
    bool prepare_plate_print(int plate_id, Slic3r::Print& print, const Slic3r::Vec3d& origin);

    // Populate Model::printSpeedMap (per-structure speeds, bed poly, exclude
    // areas) from config; needs the plate's Print config for bed_exclude_area.
    void setup_print_speed_table(Slic3r::Print& print);

    // Build the per-plate merged DynamicPrintConfig consumed by print.apply().
    // Starts from m_config and layers per-plate config overrides (curr_bed_type,
    // print_sequence, …) from PlateData on top. Key backfill and flush-matrix
    // normalization are NOT done here — they are global and run once in
    // normalize_loaded_config() before the per-plate loop. Multi-extruder /
    // wipe-tower handling is delegated to libslic3r's ToolOrdering — no
    // filament trimming is performed here (see impl for rationale).
    Slic3r::DynamicPrintConfig prepare_merged_config_for_plate(int plate_id);

    // Run Print::validate and classify all resulting warnings and errors into
    // m_stats.issues. Returns false if any validation error is fatal.
    bool run_validation(int plate_id, Slic3r::Print& print);

    // Refine a StringObjectException of type FILAMENT_NOT_MATCH_BED_TYPE by
    // substituting the user-friendly filament alias into the message text.
    // No-op for other exception types or when the preset bundle is unavailable.
    void refine_bed_mismatch_message(Slic3r::StringObjectException& ex);

    // Classify a Print::validate warning into an issue code and push it to
    // m_stats.issues. Some warning types (organic-support variable layer)
    // escalate to error level.
    void emit_validate_warning(int plate_id, const Slic3r::StringObjectException& warning);

    // Classify a Print::validate error into an issue code and push it.
    // STRING_EXCEPT_NOT_DEFINED is downgraded to a warning (desktop parity).
    // Returns true if the caller should abort slicing (fatal error).
    bool emit_validate_error(int plate_id, const Slic3r::StringObjectException& err);

    // Check print_sequence=ByObject — U1 toolchanger blocks it as a collision
    // risk. Returns false (with a fatal error pushed) when blocked.
    bool check_print_by_object(int plate_id, Slic3r::Print& print);

    // Run the filament/nozzle/bed compatibility rules (GESP, PEI adhesion,
    // nozzle mismatch) and push any resulting warnings.
    void check_filament_bed_rules(int plate_id, Slic3r::Print& print);

    // Check for high+low temperature filament mixing — U1 toolchanger cannot
    // handle the thermal difference. Returns false (with a fatal error pushed)
    // when mixing is detected.
    bool check_filament_temp_mixing(int plate_id);

    // Execute the slicing process: call print.process() to generate toolpaths.
    // Reports progress via the status callback and checks for cancellation.
    bool run_slicing(int plate_id, Slic3r::Print& print);

    // Export sliced G-code to a temp file and run GCodeProcessor analysis.
    // On success, fills result.gcode_path and result.gcode_result.
    bool export_gcode(int plate_id, Slic3r::Print& print, PlateSliceResult& result);

    // Build the thumbnail list for a plate G-code export request: locate this
    // plate's loaded thumbnail in m_plate_data and resize it to each requested
    // size. Returns an empty vector if the plate has no valid thumbnail.
    std::vector<Slic3r::ThumbnailData> make_plate_thumbnails(const Slic3r::ThumbnailsParams& params) const;

    // Collect PrintBase slicing warnings (Print + per-PrintObject step states) into
    // result.issues with message_id-aware grading: EmptyGcodeLayers -> error,
    // GcodeOverlap -> warning, others by level. Desktop CLI hard-exits on
    // EmptyGcodeLayers; the cloud engine flags the plate but keeps going.
    void collect_print_warnings(int plate_id, Slic3r::Print& print, PlateSliceResult& result);

    // Snapshot filament colour/type, nozzle diameter, filament diameter/density from
    // print.config() into result. print.config() is the merged config the slicer actually
    // applied (m_config + backfill + per-plate override, normalised by print.apply()), so
    // these values match the gcode headers and let downstream stats — which run post-slice
    // with no Print in scope — read slice-accurate values instead of the un-merged m_config.
    void capture_post_trim_config_snapshot(Slic3r::Print& print, PlateSliceResult& result);

    // Post-slicing analysis: extract filament usage, compute cost, embed
    // thumbnails, and collect per-plate issues into result.issues. plate_origin
    // (grid-layout offset of this plate) shifts G-code visualization moves
    // into plate-local space for the toolpath-outside check.
    void do_postprocessing(int plate_id, PlateSliceResult& result, const Slic3r::Vec2d& plate_origin);

    // Free G-code visualization data (moves/lines_ends) and store the result
    // into m_plate_results. Must be called after post-processing for each plate.
    void finalise_plate_result(int plate_id, PlateSliceResult& result);

    // --- Input (frozen at construction) ---
    EngineConfig m_cfg;
    std::string m_output_path;
    // Reference: temp G-code files are tracked here so the caller can clean them
    // up; the engine appends, the caller owns the storage.
    std::vector<std::string>& m_temp_files;

    // --- Loaded model & config (filled by load_3mf) ---
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

    // --- Preset system (loaded from resources/profiles/) ---
    std::unique_ptr<Slic3r::PresetBundle> m_preset_bundle;
    bool m_presets_available = false;
    std::string m_last_process_preset_name; // set by apply_process_official_preset

    // --- Per-plate results (produced during slicing) ---
    std::map<int, PlateSliceResult> m_plate_results;

    // --- Statistics & diagnostics (aggregated for JSON output) ---
    SliceOutputStats m_stats;

    // --- Run control: timeout ---
    std::chrono::steady_clock::time_point m_timeout_deadline;
    bool m_has_timeout = false;

    // --- Run control: error & exit code ---
    bool m_any_error = false;
    bool m_any_postprocess_warning = false;
    int m_error_type = EXIT_OK; // most severe exit code encountered
};
