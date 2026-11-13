// Part of the SliceEngine class, split from SliceEngine.cpp on 2026-11-13
// (pure code move, no behavior change). Domain: post-loop output and
// statistics: the all-or-nothing packaging gate, gcode.3mf assembly, per-
// plate stats aggregation, orphan-issue patching, and the final stats
// ordering / error-message derivation.

#include "SliceEngine.hpp"
#include "Utils.hpp"
#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
// Direct uses below (IWYU): boost::filesystem / system
#include <boost/filesystem.hpp>

using namespace Slic3r;

// Post-loop output finalization, in deliberate order:
//   1. the all-or-nothing packaging gate — package_output() runs only when no
//      error occurred anywhere, so one failed plate suppresses the gcode.3mf
//      for all plates (user-confirmed policy);
//   2. single-plate GCODE cleanup — that mode writes the file directly to
//      m_output_path during export_gcode, so remove it when errors occurred;
//   3. the zero-product guard — every requested plate was skipped (e.g. all
//      empty) or failed before export, so the task must not report success
//      through exit_code() with no artifact on disk.
// The guard sits AFTER the packaging / cleanup decisions on purpose:
// m_any_error feeds the packaging gate above and the single-plate GCODE
// removal — setting it earlier would drag partial output down with it.
// Partial success (some plates produced G-code) keeps rc=0: an empty plate
// among good ones is a benign skip (desktop parity, see process_plate's
// EMPTY_PLATE issue). m_error_type is only raised when still EXIT_OK so a
// more specific failure code recorded earlier (slicing=4, etc.) is not
// masked by the generic preprocess code.
//
// Debug-only exception (2026-10-20, forensics hatch in the spirit of
// ORCA_KEEP_FAILED_GCODE / P-27): with cfg.keep_failed_gcode3mf set, a run
// whose worst failure class is EXIT_POSTPROCESS_ERROR (rc=7 — every flagged
// plate still sliced and its G-code exists on disk) packages the gcode.3mf
// anyway so a human can inspect the toolpaths (confirm/refute
// TOOLPATH_OUTSIDE / TOOLPATH_CONFLICT). The CONTRACT is untouched: rc
// stays 7, the JSON stays success=false with output_file="" — the artifact
// is evidence, not delivery. rc=4/5/6 suppress output even with the flag.
void SliceEngine::finalize_output()
{
    bool has_output = !m_plate_results.empty();

    // m_error_type is a numeric-max ratchet, so == EXIT_POSTPROCESS_ERROR
    // means no failure class above 7 occurred (validation=6, slicing=4,
    // export=5 all lose the ratchet to 7 and their plates carry no G-code
    // entry anyway). Additionally require at least one plate G-code file on
    // disk: an all-empty-layer run stamps 7 too, but remove_unusable_gcode
    // already discarded every file — packaging a gcode.3mf with zero
    // embedded G-code would be a junk artifact. Plates whose file is
    // missing (empty-layer discard) are skipped per plate by the exporter's
    // exists() guard (bbs_3mf.cpp:6194/8036).
    bool keep_failed_package =
        m_cfg.keep_failed_gcode3mf && m_any_error &&
        m_error_type == EXIT_POSTPROCESS_ERROR &&
        std::any_of(m_plate_results.begin(), m_plate_results.end(),
                    [](const std::pair<const int, PlateSliceResult>& r) {
                        return boost::filesystem::exists(r.second.gcode_path);
                    });
    if (keep_failed_package)
        BOOST_LOG_TRIVIAL(warning)
            << "[keep-failed-gcode3mf] debug flag active: packaging gcode.3mf "
               "despite rc=7 (postprocess failure) for inspection — exit code "
               "and JSON report are unchanged";

    // Package output — only if no errors occurred. The gate is format-blind
    // since plate selection was removed (2026-11-11): packaging is the only
    // output path, and keep-failed semantics live on the GCODE_3MF side.
    if (has_output && (!m_any_error || keep_failed_package))
        package_output();

    if (!has_output)
    {
        // Hand-written pair, NOT mark_stage_failure: set_error_type is a
        // NUMERIC ratchet and PREPROCESS(6) > SLICING(4)/EXPORT(5), so an
        // unconditional call would overwrite the pipeline-semantic exit code
        // of an earlier slicing/export failure. This gate only labels runs
        // where nothing else went wrong.
        m_any_error = true;
        if (m_error_type == EXIT_OK)
            set_error_type(EXIT_PREPROCESS_ERROR);
    }
}

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

    // Export every plate (single-plate selection removed 2026-11-11).
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
            // Not the global record_stage_error(code,msg) overload: its
            // unconditional error_message write would clobber an earlier,
            // more specific message — this site only fills it when empty.
            BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package";
            mark_stage_failure(EXIT_EXPORT_ERROR);
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
        // Same shape as the !success branch above (conditional error_message).
        BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
        mark_stage_failure(EXIT_EXPORT_ERROR);
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

    // Shared nozzle-diameter join ("0.4" / "0.4,0.4"): the fallback below and
    // the per-plate snapshot path used to hand-roll the same loop twice
    // (smell C3 group d, 2026-11-06).
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

    // Fallback nozzle string from the raw m_config (N slots) for plates whose
    // snapshot wasn't populated (e.g. failed before export_gcode).
    std::string fallback_nozzle_str;
    if (m_config.has("nozzle_diameter"))
    {
        auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt && !nozzle_opt->values.empty())
        {
            fallback_nozzle_str = join_nozzles(nozzle_opt->values);
        }
    }

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
        // P-25 (2026-10-10): the packaged 3MF plate metadata carries the
        // G-CODE ACTUAL layer, same source as the JSON support_free flag.
        // Desktop writes the config layer here (PartPlate.cpp:5476), but that
        // field has zero readers in the entire fork tree (pure write->parse->
        // rewrite roundtrip) and cloud consumers need "did support actually
        // extrude" — config-on but self-supporting geometry must report false.
        pd->is_support_used = result.support_generated;
        pd->is_label_object_enabled = result.gcode_result.label_object_enabled;

        // Import residue guard (W2 rerun, 5215): the 3MF reader populates
        // slice_filaments_info from an embedded slice_info.config (bbs_3mf
        // read side), and sources produced by older engine versions carry
        // polluted entries there — flush-matrix virtual slots (6/7/8) with
        // real gram weights and repeated physical ids. parse_filament_info
        // only appends (no clear), so without this the packaged slice_info
        // shows stale+fresh concatenated (observed: 5 residual + 3 fresh
        // entries, sum 109.06g vs the same file's plate weight 52.97g).
        // Export statistics must describe THIS slice, never the import.
        pd->slice_filaments_info.clear();
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

// Verbatim vendor-profile version from <data_dir>/Snapmaker.json (data_dir is
// <resources>/profiles, set by slic3r_create; e.g. "02.03.03.03"). Read as a
// raw string — NOT via the preset path's Semver, whose to_string() drops the
// zero-padding. Non-critical metadata: any failure leaves the field empty
// (the JSON report then omits resources_version) and logs one warning.
static std::string read_resources_vendor_version()
{
    const std::string data_dir = Slic3r::data_dir();
    if (data_dir.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "resources_version: data_dir not set; version unavailable";
        return "";
    }
    const boost::filesystem::path vendor_json = boost::filesystem::path(data_dir) / "Snapmaker.json";
    boost::system::error_code ec;
    if (!boost::filesystem::exists(vendor_json, ec) || ec)
    {
        BOOST_LOG_TRIVIAL(warning) << "resources_version: vendor profile not found: " << vendor_json.string();
        return "";
    }
    std::ifstream in(vendor_json.string(), std::ios::binary);
    if (!in)
    {
        BOOST_LOG_TRIVIAL(warning) << "resources_version: cannot open " << vendor_json.string();
        return "";
    }
    try
    {
        nlohmann::json j;
        in >> j;
        const auto it = j.find("version");
        if (it == j.end() || !it->is_string())
        {
            BOOST_LOG_TRIVIAL(warning) << "resources_version: no string \"version\" key in " << vendor_json.string();
            return "";
        }
        return it->get<std::string>();
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(warning) << "resources_version: failed to parse " << vendor_json.string() << ": " << e.what();
        return "";
    }
}

void SliceEngine::build_statistics()
{
    // Report-time metadata read (not a slice input), so it is available on
    // every exit path — including early failures, where knowing the resources
    // version matters most for triage.
    m_stats.resources_version = read_resources_vendor_version();

    for (const auto& [plate_id, result] : m_plate_results)
    {
        SliceOutputStats::PlateStats plate_stats;
        plate_stats.plate_id = plate_id;
        assemble_plate_stats(plate_id, result, plate_stats);
        m_stats.plates.push_back(plate_stats);
    }

    patch_orphan_plate_issues();
    finalise_statistics();

    // Stamp the exit code AFTER finalise_statistics(): it (and
    // assemble_plate_stats above) are the last code paths that can raise
    // m_any_error / m_error_type. Audited 2026-10-19: every set_error_type /
    // m_any_error site lives before or inside build_statistics(); between
    // this stamp and every entry point's return there is only pure reading.
    // A future guard appended AFTER build_statistics() would silently
    // desynchronise stats.exit_code from the returned rc — the test_c_api
    // [exit_code] cases assert j["exit_code"] == rc to catch exactly that.
    m_stats.exit_code = exit_code();
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
        mark_stage_failure(EXIT_PREPROCESS_ERROR);
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
    plate_stats.print_time = normal_mode.time - normal_mode.prepare_time;
    if (!std::isfinite(plate_stats.print_time))
    {
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << " print time is non-finite (" << normal_mode.time
                                   << " - " << normal_mode.prepare_time << "), falling back to 0";
        plate_stats.print_time = 0.0f;
    }

    plate_stats.total_filament_m = result.total_used_filament;
    plate_stats.total_filament_g = result.total_weight;
    plate_stats.support_generated = result.support_generated;

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

        // Flow type declaration, joined in the same extruder_id index space
        // as type/colour above. Deliberately NO m_config fallback: the value
        // domain has two members, so "standard" (the in-class default) IS the
        // fallback, and m_config's index space can differ from the post-apply
        // one under the single-extruder remap (see the colours comment).
        // to_string() is bounds-checked — a raw int outside the enum renders
        // as "standard".
        if (ext_sz < result.filament_volume_types.size())
            detail.volume_type = to_string(FilamentVolumeType(result.filament_volume_types[ext_sz]));

        // Support-material declaration, joined in the same extruder_id index
        // space as volume_type above. Same deliberately-no-m_config-fallback
        // rationale (single-extruder remap index space); the in-class default
        // false is the collapse value for missing/out-of-range slots.
        if (ext_sz < result.filament_is_supports.size())
            detail.is_support = result.filament_is_supports[ext_sz] != 0;

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
    // Severity-aware (benign-skip semantics): only error / serious_warning
    // orphans fabricate a failed placeholder plate — those indicate a plate
    // that genuinely failed and must flip overall success in
    // finalise_statistics. Warning/tip orphans (EMPTY_PLATE,
    // NO_PRINTABLE_OBJECTS) are benign skips: fabricating a success=false
    // placeholder for them would drag a 9/10-good multi-plate task's overall
    // success to false while rc stays 0 — an inconsistent report. They stay
    // in the global issue list with their plate_id, so callers can still see
    // which plate was skipped.
    std::set<int> plates_with_results;
    // Fabricated placeholder plates, by plate_id → index into m_stats.plates.
    // Collect-all (2026-11-03): a condemned plate can now carry several error
    // issues (build volume + config checks); every subsequent orphan of the
    // same plate is appended to its placeholder instead of only the first one
    // landing there. Warning/tip orphans stay global-only, as before.
    std::map<int, size_t> placeholder_index;
    for (const auto& p : m_stats.plates)
        plates_with_results.insert(p.plate_id);

    for (const auto& issue : m_stats.issues)
    {
        if (issue.plate_id < 0 || plates_with_results.find(issue.plate_id) != plates_with_results.end())
            continue;

        if (issue.level != IssueLevel::error && issue.level != IssueLevel::serious_warning)
            continue;

        if (auto it = placeholder_index.find(issue.plate_id); it != placeholder_index.end())
        {
            m_stats.plates[it->second].issues.push_back(issue);
            continue;
        }

        SliceOutputStats::PlateStats failed_plate;
        failed_plate.plate_id = issue.plate_id;
        failed_plate.success = false;
        failed_plate.plate_count = static_cast<int>(m_plate_data.size());
        failed_plate.issues.push_back(issue);
        // NOTE: not inserted into plates_with_results — that set guards REAL
        // result plates only. Registering the placeholder there would make
        // the top guard swallow this plate's subsequent orphan issues before
        // the append branch above could collect them.
        placeholder_index.emplace(issue.plate_id, m_stats.plates.size());
        m_stats.plates.push_back(std::move(failed_plate));
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
                mark_stage_failure(EXIT_PREPROCESS_ERROR);
                break;
            }
        }
    }

    if (m_stats.success || !m_stats.error_message.empty())
        return;

    if (m_stats.plates.empty())
    {
        m_stats.error_message = "No plates completed successfully";
        // Zero-product observability (P-23, production 1e9d9919...): JSON-only
        // consumers read issue level, not error_message — before this, the
        // empty-plates branch was the only error_message setter that pushed no
        // issue, so exit 6 shipped with a warnings-only issue list.
        //
        // Context gate — m_any_error is set by run()'s zero-product guard
        // right before build_statistics() at run() end, so here it
        // distinguishes "a slicing run produced nothing" from the non-run()
        // contexts (run_geometry_preprocess_only / run_preset_substitution_
        // only) where an empty plates list is BY DESIGN and must not
        // fabricate a failure issue. Flags are NOT set here: that is run()'s
        // guard's job (its ordering relative to the packaging gate is
        // deliberate, see the guard's comment).
        //
        // Suppressed when a more specific error issue already attributes the
        // failure (e.g. PLATELESS_NO_PRINTABLE_INSTANCES): the generic code
        // would add no information. Global plate_id=-1 keeps
        // patch_orphan_plate_issues from fabricating a placeholder plate.
        if (m_any_error)
        {
            bool has_error_issue = false;
            for (const auto& issue : m_stats.issues)
            {
                if (issue.level == IssueLevel::error || issue.level == IssueLevel::serious_warning)
                {
                    has_error_issue = true;
                    break;
                }
            }
            if (!has_error_issue)
            {
                m_stats.issues.push_back(make_error(-1, "NO_PLATES_PRODUCED", m_stats.error_message));
                std::stable_sort(m_stats.issues.begin(), m_stats.issues.end(), by_severity);
            }
        }
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
