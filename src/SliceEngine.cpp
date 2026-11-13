// Entry points and pipeline orchestration for the SliceEngine class: the
// run()/run_*() variants, their shared config/preset and geometry prefixes,
// stage-error recording, and exit-code determination. The class itself is
// declared once in SliceEngine.hpp; its implementation is split per domain
// across SliceEngine{Load,Presets,Geometry,Plate,Output}.cpp (2026-11-13,
// pure code move, no behavior change).
#include "SliceEngine.hpp"
#include "GeometryCheck.hpp"

#include <exception>
#include <typeinfo>

#include <boost/log/trivial.hpp>

using namespace Slic3r;

SliceEngine::SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files)
    : m_cfg(cfg), m_temp_files(temp_files)
{
    // 3MF loading lazily creates a per-model backup dir under temporary_dir()
    // (Model::get_backup_path). Without this the empty default resolves to
    // "/snapmaker_orca_model" at filesystem root and every load fails.
    // m_cfg.temp_dir is the engine's canonical temp location; fall back to
    // the system temp dir when the caller did not set one.
    if (!m_cfg.temp_dir.empty())
        Slic3r::set_temporary_dir(m_cfg.temp_dir);
    else
        Slic3r::set_temporary_dir(boost::filesystem::temp_directory_path().string());
}

SliceEngine::~SliceEngine()
{
    release_PlateData_list(m_plate_data);
}

// Shared 7-step config/preset prefix of the three entry points (contract on
// the declaration in SliceEngine.hpp). Defined directly above run() so the
// call order reads top-down alongside the stages it composes.
bool SliceEngine::run_config_and_preset_prefix()
{
    if (!load_3mf())
        return false;

    // Block slicing if printer model is not Snapmaker U1.
    // Moved before preset loading to fail fast — this check reads only
    // m_config (populated during 3MF load) and has no preset dependency.
    if (!validate_printer_model())
        return false;

    // Collect config warnings (never blocks the pipeline).
    collect_config_warnings();

    // Load vendor preset JSONs from resources/profiles/. Required for the
    // apply_*_official_preset stages below.
    load_system_presets();

    // Hard precondition (P-09): must abort before init_print dereferences
    // m_preset_bundle. See ensure_presets_available for the full contract.
    if (!ensure_presets_available())
        return false;

    if (!apply_preset_substitution())
        return false;

    // Runs after the substitution stages: both fixes guard against libslic3r
    // SEGVs and are independent of what substitution did.
    normalize_loaded_config();

    return true;
}

bool SliceEngine::run()
{
    // Shared config/preset prefix with run_preset_substitution_only() /
    // run_geometry_preprocess_only(); on failure only stamp JSON statistics
    // and stop (each entry point owns its own build_statistics call).
    if (!run_config_and_preset_prefix())
    {
        build_statistics();
        return false;
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
            // --- Plate membership rebuild (P-24 always-rebuild, before per-plate loop) ---
            // Runs after apply_preset_substitution() above so printable_area
            // is the official U1 value, and before process_plate — the first
            // membership consumer (mark_printable_instances). Ordering
            // against bake_instance_z_into_mesh inside run_geometry_prefix
            // below is irrelevant: it only moves Z, and membership is decided
            // by the XY intersection test.
            rebuild_plate_membership();

            // Geometry half (defect checks + Z-bake + arrange order + extruder
            // params), shared with run_geometry_preprocess_only().
            run_geometry_prefix();

            decode_plate_thumbnails();

            resolve_output_path();

            process_requested_plates();

            finalize_output();
        }
        catch (const std::exception& e)
        {
            record_pipeline_exception("slicing pipeline", "slicing", &e);
        }
        catch (...)
        {
            record_pipeline_exception("slicing pipeline", "slicing", nullptr);
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
    // No geometry checks, slicing, or export. The sequence itself lives in
    // run_config_and_preset_prefix(), shared with run() and
    // run_geometry_preprocess_only().
    if (!run_config_and_preset_prefix())
    {
        build_statistics();
        return false;
    }

    // m_config now holds the substitution result; run()'s later stages do not
    // modify the preset keys, so callers can read it via config().
    build_statistics();
    return !m_any_error;
}

bool SliceEngine::run_geometry_preprocess_only()
{
    // Config + geometry-preprocessing prefix of run(): everything in run() up
    // to and including setup_extruder_params (via run_geometry_prefix), minus
    // rebuild_plate_membership, decode_plate_thumbnails, resolve_output_path,
    // the per-plate slicing loop, package_output, and export. The geometry
    // stages mutate m_model in place (Z baked into mesh, arrange_order
    // stamped); after return it is readable via model(). No slicing, no
    // export. The config half lives in run_config_and_preset_prefix(),
    // shared with run() and run_preset_substitution_only().
    if (!run_config_and_preset_prefix())
    {
        build_statistics();
        return false;
    }

    // --- Geometry-half preprocessing (mirrors run()) ---
    if (!validate_input())
    {
        build_statistics();
        return false;
    }

    try
    {
        run_geometry_prefix();
    }
    catch (const std::exception& e)
    {
        record_pipeline_exception("geometry preprocessing", "geometry preprocessing", &e);
    }
    catch (...)
    {
        record_pipeline_exception("geometry preprocessing", "geometry preprocessing", nullptr);
    }

    build_statistics();
    return !m_any_error;
}

// ============================================================================
// Shared geometry-half stages + shared pipeline catch reporter
// (smell C1, 2026-11-06: the four geometry stages and the catch-pair were
// duplicated verbatim between run() and run_geometry_preprocess_only())
// ============================================================================

void SliceEngine::run_geometry_prefix()
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
    // validate_input), writes a static Model member, no Print or plate
    // dependency -- so it belongs here, not in the per-plate loop where it
    // was previously re-run with identical results N times.
    setup_extruder_params();
}

void SliceEngine::record_pipeline_exception(const char* stage_log, const char* stage_msg, const std::exception* e)
{
    // Order (log → flags → issue push) matches the pre-extraction catch blocks
    // exactly; the issue messages are byte-identical and JSON-visible.
    if (e != nullptr)
    {
        BOOST_LOG_TRIVIAL(error) << "Unhandled exception in " << stage_log << ": " << e->what();
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        m_stats.issues.push_back(make_error(
            -1, "INTERNAL_ERROR",
            std::string("An unexpected internal error occurred during ") + stage_msg +
                ". Please try again or contact support."));
    }
    else
    {
        BOOST_LOG_TRIVIAL(error) << "Unhandled non-standard exception in " << stage_log;
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        m_stats.issues.push_back(make_error(
            -1, "INTERNAL_FATAL", std::string("Unhandled unknown exception in ") + stage_log));
    }
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

void SliceEngine::record_stage_error(int exit_type, const std::string& code, const std::string& msg)
{
    BOOST_LOG_TRIVIAL(error) << msg;
    m_any_error = true;
    set_error_type(exit_type);
    m_stats.error_message = msg;
    m_stats.issues.push_back(make_error(-1, code, msg));
}

void SliceEngine::mark_stage_failure(int exit_type)
{
    m_any_error = true;
    set_error_type(exit_type);
}

void SliceEngine::record_stage_error(int exit_type, Issue issue)
{
    mark_stage_failure(exit_type);
    m_stats.issues.push_back(std::move(issue));
}

// Stage 4 driver: run the per-plate slicing loop over the requested plate
// set. Every plate in the 3MF is processed (plate selection removed
// 2026-11-11). Results accumulate in m_plate_results.
void SliceEngine::process_requested_plates()
{
    // Collect plates to process (internal plate_index is 0-based)
    std::vector<int> plates_to_process;
    for (const auto& pd : m_plate_data)
        plates_to_process.push_back(pd->plate_index);

    // Process each plate
    for (int plate_id : plates_to_process)
    {
        process_plate(plate_id);
    }
}

// ============================================================================
// Stage 5: Package output as gcode.3mf
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
    // Post-processing warnings are non-fatal (desktop parity): the G-code
    // has been generated and remains usable, so the exit code stays EXIT_OK.
    return EXIT_OK;
}

// ============================================================================
// Stage 6: Build statistics for JSON output
// ============================================================================

