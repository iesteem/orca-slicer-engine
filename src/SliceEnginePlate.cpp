// Part of the SliceEngine class, split from SliceEngine.cpp on 2026-11-13
// (pure code move, no behavior change). Domain: per-plate sub-stages (the
// header's "Per-plate sub-stages" section): build-volume checks, Print
// preparation and validation, slicing, G-code export, postprocessing
// (toolpath-outside attribution), and per-plate result finalisation.

#include "SliceEngine.hpp"
#include "BuildVolumeClassify.hpp"
#include "OutsideAttribution.hpp"
#include "SlicingDeadline.hpp"
#include "Utils.hpp"
#include "ValidateClassify.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/FilamentHotBedNozzleRules.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
// Direct uses below (IWYU): boost::filesystem / system
#include <boost/filesystem.hpp>

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

} // namespace

// ----------------------------------------------------------------------------
// File-local free-function forward declarations.
//
// These helpers do not touch any SliceEngine member state (pure input→output),
// so they are kept as file-local `static` functions rather than members. They
// are defined further down but declared here because call sites in member
// functions appear before their definitions.
// ----------------------------------------------------------------------------
static bool  all_gcode_layers_valid(const PlateSliceResult& slice_result);
static void  remove_unusable_gcode(int plate_id, PlateSliceResult& slice_result);

void SliceEngine::process_plate(int plate_id)
{
    // Guaranteed by run(): apply_printer_official_preset() must succeed first
    // (which in turn requires bundle availability). Not assert-guarded —
    // structural contract, no-op in Release.

    // --- Identify this plate's instances and mark printability ---
    std::set<int> identify_ids = collect_plate_instance_ids(plate_id);
    if (!mark_printable_instances(identify_ids))
    {
        // Benign skip (desktop parity): an empty plate among good ones is not
        // a failure. Warning-level on purpose — patch_orphan_plate_issues
        // only fabricates failed placeholder plates for error-level issues,
        // so this stays visible in the JSON issue list (with its plate_id)
        // without flipping the multi-plate task's overall success. The
        // zero-product rc guard in run() covers the single-plate / all-empty
        // cases where this skip leaves the task with no output at all.
        m_stats.issues.push_back(make_warning(plate_id, "EMPTY_PLATE",
            "Plate " + std::to_string(plate_id + 1) + " contains no printable objects and was skipped"));
        return;
    }

    // Plate origin = this plate's grid-layout offset. Shared by two downstream
    // steps: run_build_volume_check (translates instances into plate-local
    // coordinates) and prepare_plate_print (positions the plate in global space). Plate
    // dimensions are derived from printable_area inside setup_print_origin.
    Vec3d origin = setup_print_origin(plate_id);
    // Collect-all contract (2026-11-03): a build-volume failure condemns the
    // plate for slicing but no longer skips the independent config checks —
    // their issues must still land in the JSON alongside the build-volume
    // ones. The volume verdict only skips Print::validate and everything
    // after it (derived-error suppression: out-of-volume geometry is polluted
    // input for validate's height/collision checks, whose results on it
    // would be noise, not independent findings).
    bool volume_ok = run_build_volume_check(plate_id, identify_ids, origin);

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

    if (!volume_ok)
    {
        // Condemned plate: its build-volume issues are already collected and
        // it will not be sliced. Run only the independent config checks (via
        // prepare, which populates print.config()). The continuation is
        // guarded on purpose: print.apply on condemned geometry must not be
        // able to escalate the clean rc=6 verdict into run()'s rc=4
        // INTERNAL_ERROR — the plate is already fully reported, so log and
        // keep the preprocess verdict (documented swallow, exception-safety
        // harness §8: catch-all with logging + rationale).
        try
        {
            if (prepare_plate_print(plate_id, print, origin))
                run_config_checks(plate_id, print);
        }
        catch (const std::exception& e)
        {
            BOOST_LOG_TRIVIAL(error)
                << "Plate " << plate_id << ": config checks after build-volume failure threw "
                   "(plate already condemned, verdict unchanged): "
                << e.what();
        }
        catch (...)
        {
            BOOST_LOG_TRIVIAL(error)
                << "Plate " << plate_id << ": config checks after build-volume failure threw a "
                   "non-standard exception (plate already condemned, verdict unchanged)";
        }
        return;
    }

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
        // A synthesized single plate with zero members means the whole file
        // has no printable instance (e.g. every item is printable="0") —
        // desktop refuses to slice such a file upfront, so hard-fail here
        // instead of returning a silent exit-0-with-no-output.
        //
        // Real (metadata-backed) empty plates keep the historical warning +
        // skip: one empty plate must not sink the other plates (precedent
        // P-19, option A on feature-2.2.0).
        if (m_synthesized_single_plate)
        {
            BOOST_LOG_TRIVIAL(error) << "Synthesized single plate has no printable instances";
            m_any_error            = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
            m_stats.issues.push_back(
                make_error(-1, "PLATELESS_NO_PRINTABLE_INSTANCES",
                           "The 3MF file has no plate metadata and none of its objects is printable; "
                           "there is nothing to slice."));
            return false;
        }
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
                // Desktop blocks slicing for off-plate objects: a fully-outside
                // instance makes on_slice_button_status disable the slice
                // button (GLCanvas3D.cpp:2876-2877 gates on contained_min_one,
                // which requires EVERY printable instance to be at least
                // partly inside). The cloud engine has no "grey out and skip"
                // UI concept, so escalate to a blocking error (previously a
                // warning here let slicing proceed and the failure surfaced
                // later as TOOLPATH_OUTSIDE, exit 7 instead of exit 6).
                log_plate_message("[Pre-processing]", "ERROR", plate_id,
                                  "Object \"" + obj->name + "\" is completely outside the build volume.");
                has_partly_outside = true;
                m_stats.issues.push_back(make_error(
                    plate_id, "BUILD_VOLUME_FULLY_OUTSIDE",
                    "Object \"" + obj->name + "\" is completely outside the build volume and will not be printed",
                    obj->name,
                    "Move the object back onto the build plate before slicing."));
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
        mark_stage_failure(EXIT_PREPROCESS_ERROR);
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
    // Matches desktop 3DScene.cpp:1105-1122, applied to the *effective*
    // per-slot lift type: machine z_hop_types merged with the
    // filament_z_hop_types override, the same merge Print::apply's
    // print_config_diffs performs before G-code generation (P-35). Reading
    // the raw machine key reports a false SPIRAL_LIFT_NEAR_BOUNDARY for jobs
    // whose filament override disables spiral lift (SnapSpeed "Slope Lift"
    // over U1 machine "Auto Lift" base) — after the P-35 GCode.cpp fix the
    // runtime never spirals on those slots.
    bool spiral_lift_active = false;
    if (m_config.has("z_hop_types"))
    {
        auto* zht_opt = m_config.option<ConfigOptionEnumsGeneric>("z_hop_types");
        if (zht_opt)
        {
            // Own the clone via unique_ptr: apply_override() throws on type
            // mismatch / nullable reject and clone() can itself fail
            // allocation — a raw pointer leaks on any of those paths (RAII
            // harness, resource bound at acquisition). Deleting through the
            // base pointer is well-defined: ConfigOption's destructor is
            // virtual (Config.hpp), the same ownership fork's own
            // ConfigOptionUniquePtr relies on.
            std::unique_ptr<ConfigOption> effective(zht_opt->clone());
            const ConfigOption* fil_override = m_config.option("filament_z_hop_types");
            if (fil_override != nullptr && !fil_override->is_nil())
                static_cast<ConfigOptionVectorBase*>(effective.get())->apply_override(fil_override);
            for (int v : static_cast<ConfigOptionEnumsGeneric*>(effective.get())->values)
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
    record_stage_error(EXIT_SLICING_ERROR, make_error(plate_id, "SLICING_TIMEOUT",
                                        "Slicing timed out. The model may be too complex. If you believe this is "
                                        "an error, please submit an appeal for review."));
    return false;
}

void SliceEngine::init_print(Print& print)
{
    print.set_status_callback(
        [&print, this](const PrintBase::SlicingStatus& s)
        {
            default_status_callback(s, &print, &m_cfg.cancel_file, m_cfg.cancel_requested);
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

    // Slicing-only auto-brim zero (desktop 2.4.0 motion parity). In auto mode
    // the fork's libslic3r treats brim_width as a fallback that draws a
    // user-width brim whenever the auto algorithm decides "no brim" — desktop
    // Orca 2.4.0 prints nothing in that case (verified on cube / 0.6mm-nozzle
    // fixtures, 2026-09-18). Zeroing only this slicing copy keeps motion
    // identical to the previous apply_auto_brim_fallback() behaviour while
    // leaving m_config's brim_width intact for the write-back paths (gcode
    // header echo restored by restore_brim_width_echo, output 3mf
    // project_settings.config written from m_config). Must run BEFORE the
    // per-plate overrides below so a plate-level brim_width override still
    // reaches the print, matching the old Stage-1.4 ordering.
    if (auto* bt = merged_config.option<ConfigOptionEnum<BrimType>>("brim_type", false);
        bt && bt->value == btAutoBrim)
    {
        merged_config.set_key_value("brim_width", new ConfigOptionFloat(0));
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

void SliceEngine::setup_print_speed_table(Print& print)
{
    Model::setPrintSpeedTable(m_config, print.config());
}

bool SliceEngine::run_validation(int plate_id, Print& print)
{
    // Collect-all contract (2026-11-03): a fatal Print::validate error no
    // longer aborts the remaining checks. Every independent check runs and
    // pushes its own issue, so the JSON carries ALL preprocessing validation
    // errors; the aggregate verdict (return false) merely blocks slicing.
    // Exception-safety note: each sub-check already owns its side effects
    // (issue push + m_any_error + set_error_type) before returning, so
    // continuing after one failed check cannot lose or double-record state.
    bool ok = true;

    StringObjectException warning;
    StringObjectException err = print.validate(&warning, nullptr, nullptr);

    refine_bed_mismatch_message(warning);
    refine_bed_mismatch_message(err);

    if (!warning.string.empty())
        emit_validate_warning(plate_id, warning);

    if (!err.string.empty())
        if (!emit_validate_error(plate_id, err))
            ok = false;

    if (!run_config_checks(plate_id, print))
        ok = false;

    return ok;
}

bool SliceEngine::run_config_checks(int plate_id, Print& print)
{
    // The three config-only checks, all executed regardless of each other's
    // outcome (see header). check_filament_bed_rules only emits warnings and
    // has no verdict to fold in.
    bool ok = check_print_by_object(plate_id, print);
    check_filament_bed_rules(plate_id, print);
    if (!check_filament_temp_mixing(plate_id))
        ok = false;
    return ok;
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

// Shared tail of emit_validate_warning / emit_validate_error (smell C3,
// 2026-11-06): build the final message, fill the canned suggestion for the
// two fixed-suggestion codes, push the Issue at the classified level, and set
// the error flag + exit type when escalated. Byte-for-byte the logic both
// functions used to duplicate; does NOT touch m_stats.error_message.
void SliceEngine::push_classified_issue(int plate_id, const orca::ExceptionClassification& cls,
                                         const std::string& raw_text, const std::string& obj_name,
                                         const std::string& opt_hint)
{
    std::string message = cls.fixed_message.empty() ? (raw_text + opt_hint) : cls.fixed_message;

    if (cls.level == IssueLevel::error)
    {
        std::string suggestion = cls.code == "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT" ? orca::kOrganicFixedSuggestion
                      : cls.code == "PRIME_TOWER_VARIABLE_LAYER_HEIGHT"          ? orca::kPrimeTowerFixedSuggestion
                                                                                  : std::string{};
        m_stats.issues.push_back(make_error(plate_id, cls.code, message, obj_name, suggestion));
        mark_stage_failure(EXIT_PREPROCESS_ERROR);
    }
    else
    {
        m_stats.issues.push_back(make_warning(plate_id, cls.code, message, obj_name));
    }
}

void SliceEngine::emit_validate_warning(int plate_id, const StringObjectException& warning)
{
    auto [obj_name, opt_hint] = format_exception_context(warning);
    BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << ": " << warning.string << obj_name << opt_hint;

    // Pure classification (warning-path code table) lives in
    // orca::classify_validate_exception (unit-tested). NOTE: the ORGANIC type
    // escalates to error level on the warning path too.
    auto cls = orca::classify_validate_exception(static_cast<int>(warning.type), /*is_error_path=*/false, warning.string);
    push_classified_issue(plate_id, cls, warning.string, obj_name, opt_hint);
}

bool SliceEngine::emit_validate_error(int plate_id, const StringObjectException& err)
{
    auto [obj_name, opt_hint] = format_exception_context(err);
    // Pure classification (error-path code table, NOT_DEFINED substring match,
    // continue/abort contract) lives in orca::classify_validate_exception
    // (unit-tested). This thin wrapper adds the two-path logging; Issue
    // construction and member side effects live in push_classified_issue.
    auto cls = orca::classify_validate_exception(static_cast<int>(err.type), /*is_error_path=*/true, err.string);

    // NOT_DEFINED (continue_slicing=true) is logged at warning level, matching
    // the original code; all other error-path types are logged at error level.
    // (BOOST_LOG_TRIVIAL needs a literal severity name, so branch explicitly.)
    if (cls.continue_slicing)
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;
    else
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": " << err.string << obj_name << opt_hint;

    push_classified_issue(plate_id, cls, err.string, obj_name, opt_hint);
    return cls.continue_slicing;
}

bool SliceEngine::check_print_by_object(int plate_id, Print& print)
{
    // U1 toolchanger cannot safely print by object — collision risk during
    // tool switches. Block at validation rather than letting it reach slicing.
    if (print.config().print_sequence.value != PrintSequence::ByObject)
        return true;

    record_stage_error(EXIT_PREPROCESS_ERROR, make_error(
        plate_id, "PRINT_BY_OBJECT_CAUTION",
        "Print-by-object may cause the print head to collide with printed parts during switching.",
        "" /*object_name*/,
        "In Snapmaker Orca, switch to print-by-layer or ensure sufficient clearance between objects."));
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

// Desktop parity helper (Plater.cpp collect_filament_slots_from_model_config,
// :461-484 in the current checkout): the filament slots referenced by ONE
// config layer (object, volume, or layer-height range) are the primary
// extruder plus the six per-feature filament keys. Values outside
// [1, num_filaments] (including the "0 = inherit default" sentinel) are
// ignored, matching the desktop collector exactly. Takes the ModelConfig
// base so ModelConfigObject call sites bind by upcast and layer-range
// ModelConfig values fit unchanged.
static void collect_filament_slots_from_model_config(const ModelConfig& config,
                                                     int num_filaments,
                                                     std::set<int>& used_slots_0_based)
{
    static const std::vector<const char*> feature_keys = {
        "support_filament",
        "support_interface_filament",
        "wall_filament",
        "sparse_infill_filament",
        "solid_infill_filament",
        "wipe_tower_filament",
    };
    if (config.has("extruder")) {
        const int extruder_id = config.extruder();
        if (extruder_id >= 1 && extruder_id <= num_filaments)
            used_slots_0_based.insert(extruder_id - 1);
    }
    for (const char* key : feature_keys) {
        if (config.has(key)) {
            const int val = config.opt_int(key);
            if (val >= 1 && val <= num_filaments)
                used_slots_0_based.insert(val - 1);
        }
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
    //
    // Collection sources, in desktop order (Plater.cpp:22250-22315):
    //   (i) plate config — intentionally skipped: the BBS 3MF plate-config
    //       whitelist (bbs_3mf.cpp plate-metadata parser) accepts no
    //       filament keys, so this source is inexpressible in cloud input;
    //  (ii) object config, (iii) every volume's config — modifiers included
    //       — plus per-volume extruders: collected in (a) below;
    //  (ii+) layer-height-range configs — ENGINE-ONLY source (P-33): the
    //       desktop collector never reads layer_config_ranges (upstream
    //       issue#5 family); the engine deliberately collects them so
    //       range-band extruder switches are mixing-checked. Divergence
    //       is stricter-than-desktop by design; see (a) below;
    //  (iv) global always/conditional keys: (b)/(c) below.
    int num_filaments = 0;
    auto* ftype_opt = m_config.option<ConfigOptionStrings>("filament_type");
    if (ftype_opt)
        num_filaments = static_cast<int>(ftype_opt->values.size());
    if (num_filaments <= 1)
        return true;

    std::set<int> used_slots;

    // (a) Collect filament slots from the objects printed on this plate
    // (desktop sources ② and ③): object-level config first, then EVERY
    // volume's config — no is_model_part() filter, so modifier overrides
    // are seen — plus per-volume extruders (mmu segmentation, extruder_id).
    // The old implementation only read model-part get_extruders(), which
    // never looks at feature keys and skips modifiers entirely, so object-
    // level / part-level feature filaments and modifier extruders were
    // invisible to the mixing check (review 2026-09-16, item C1).
    //
    // NOTE: Per-plate filtering relies on mark_printable_instances(plate_ids)
    // in process_plate having marked off-plate instances printable=false.
    // The is_printable() guard inside this loop is the per-plate filter —
    // equivalent to desktop model_object_is_on_plate().
    bool uses_default_extruder = false;
    for (ModelObject* obj : m_model.objects)
    {
        bool on_plate = false;
        for (ModelInstance* inst : obj->instances)
            if (inst->is_printable()) { on_plate = true; break; }
        if (!on_plate)
            continue;

        collect_filament_slots_from_model_config(obj->config, num_filaments, used_slots);
        // Layer-height ranges (engine-first addition, P-33): each range's
        // ModelConfig overrides the per-feature filament keys for its z span,
        // so a band can switch the print to another extruder mid-object.
        // Desktop Plater::check_filament_temp_mixing() never reads
        // layer_config_ranges (upstream issue#5 family) and waves range-only
        // mixing through; the engine deliberately diverges stricter — the
        // printed output really does change tools at the band boundary
        // (repro fixture: T0+T1 toolchanges in the gcode while exit stays 0),
        // so a high/low pair reachable this way is blocked like every other
        // source. Same collector, same [1, num_filaments] guard.
        for (const auto& range_entry : obj->layer_config_ranges)
            collect_filament_slots_from_model_config(range_entry.second, num_filaments, used_slots);
        // An object without an explicit "extruder" key in its config
        // (or with extruder==0) inherits the global default.  This
        // matches the desktop Plater::check_filament_temp_mixing logic.
        if (!obj->config.has("extruder") || obj->config.extruder() == 0)
            uses_default_extruder = true;

        for (ModelVolume* vol : obj->volumes)
        {
            collect_filament_slots_from_model_config(vol->config, num_filaments, used_slots);
            for (int eid : vol->get_extruders())
                if (eid >= 1 && eid <= num_filaments)
                    used_slots.insert(eid - 1);
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
    record_stage_error(EXIT_PREPROCESS_ERROR, make_error(plate_id, "FILAMENT_TEMP_MIXING", msg));
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
        mark_stage_failure(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const SlicingError& ex)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << ex.what();
        record_stage_error(EXIT_SLICING_ERROR, make_error(
            plate_id, "SLICING_FATAL_ERROR",
            "A fatal slicing error occurred while processing this plate. "
            "The model geometry or print settings may be incompatible with the selected configuration."));
        return false;
    }
    catch (const CanceledException&)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << ": Slicing was cancelled.";
        record_stage_error(EXIT_SLICING_ERROR, make_error(plate_id, "SLICING_CANCELLED",
                                            "Slicing was cancelled before completion. "
                                            "This may be caused by an external cancellation request or a resource limit."));
        return false;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing exception: " << e.what();
        record_stage_error(EXIT_SLICING_ERROR, make_error(plate_id, "SLICING_EXCEPTION",
                       "An unexpected internal error interrupted slicing for this plate. "
                       "This is not caused by the model — please try again or contact support."));
        return false;
    }

    return true;
}

// Free the G-code visualization data the cloud engine never uses.
// GCodeProcessorResult::moves (MoveVertex) and lines_ends (size_t) store
// every G-code move vertex for the desktop GUI — hundreds of MB per plate
// (see finalise_plate_result for the bad_alloc precedent). Shared by the
// normal path and export_gcode's catch (smell C5, 2026-11-06: the error
// path used to copy the whole result into m_plate_results WITHOUT this
// cleanup, so a failed plate retained its moves vectors until process exit
// — the exact memory pattern the normal path's cleanup exists to prevent).
// Safe inside a catch handler: clear() on vectors of trivially destructible
// elements cannot throw, and shrink_to_fit() on the now-empty vector only
// deallocates (no allocation, no bad_alloc escape).
// File-local free function: no member state.
static void clear_intermediate(PlateSliceResult& result)
{
    result.gcode_result.moves.clear();
    result.gcode_result.moves.shrink_to_fit();
    result.gcode_result.lines_ends.clear();
    result.gcode_result.lines_ends.shrink_to_fit();
}

bool SliceEngine::export_gcode(int plate_id, Print& print, PlateSliceResult& result)
{
    // All-plates mode only (plate selection removed 2026-11-11): every
    // plate's gcode goes to a temp file that packaging later embeds into
    // the .gcode.3mf package.
    std::string gcode_output = m_cfg.temp_dir + "/plate_" + std::to_string(plate_id) + ".gcode";
    m_temp_files.push_back(gcode_output);

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code for plate " << plate_id << "...";

    try
    {
        GCodeProcessor::s_IsBBLPrinter = print.is_BBL_printer();

        auto thumbnail_cb = [this](const Slic3r::ThumbnailsParams& params) {
            return make_plate_thumbnails(params);
        };

        std::string exported = print.export_gcode(gcode_output, &result.gcode_result, thumbnail_cb);
        result.gcode_path = exported;

        restore_brim_width_echo(result.gcode_path);

        // Post-processing scripts are disabled in cloud mode to prevent
        // remote code execution via user-uploaded 3MF files.
        // run_post_process_scripts(result.gcode_path, print.full_print_config());

        collect_print_warnings(plate_id, print, result);

        const PrintStatistics& ps = print.print_statistics();
        result.total_weight = ps.total_weight;
        // G-code actual layer: any support extrusion volume (roles
        // erSupportMaterial/Interface/Transition, incl. raft) means support
        // structures were really generated. Drives the JSON support_free flag
        // and (P-25) the packaged 3MF plate metadata support_used relay.
        double support_volume = 0.0;
        for (const auto& [extruder, vol] :
             result.gcode_result.print_statistics.support_volumes_per_extruder)
            support_volume += vol;
        result.support_generated = support_volume > 0.0;
        result.total_used_filament = ps.total_used_filament;
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
        // Hand-written triplet, NOT record_stage_error(int, Issue): the issue
        // goes to this plate's result list (consumed by the per-plate JSON
        // report), not the global m_stats.issues list. Same for the other
        // result.issues failure sites (postprocess empty-layers, toolpath
        // conflict) — the helpers push globally.
        result.issues.push_back(
            make_error(plate_id, "GCODE_EXPORT_ERROR",
                       "Failed to write G-code output. "
                       "Check that the output directory exists and has sufficient disk space."));
        mark_stage_failure(EXIT_EXPORT_ERROR);
        // Same visualization-data cleanup the normal path applies in
        // finalise_plate_result: a failed export can leave partial moves/
        // lines_ends in gcode_result, and copying them wholesale into
        // m_plate_results would retain hundreds of MB until process exit
        // (smell C5 — this catch used to bypass the cleanup).
        clear_intermediate(result);
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

void SliceEngine::restore_brim_width_echo(const std::string& gcode_path)
{
    // Only auto mode diverges: prepare_merged_config_for_plate zeroes
    // brim_width on the slicing copy, so the config echo (generated from the
    // print config inside GCode::_do_export) says 0 while desktop 2.4.0
    // echoes the project value with identical motion. Other brim types reach
    // the print config untouched and need no rewrite.
    auto* bt = m_config.option<ConfigOptionEnum<BrimType>>("brim_type", false);
    if (!bt || bt->value != btAutoBrim)
        return;
    const auto* bw = m_config.option<ConfigOptionFloat>("brim_width", false);
    if (!bw || bw->value == 0)
        return;  // 0 is what the zeroed slicing copy echoes: nothing to restore

    // Best-effort cosmetics, never escalate: export_gcode's catch would flag
    // GCODE_EXPORT_ERROR and the all-or-nothing packaging would drop the
    // whole job's output over a comment line. Every failure below degrades
    // to a log line and leaves the original gcode untouched — including the
    // expected-string construction, which allocates and must stay inside
    // the try so a bad_alloc lands in the catch instead of escaping.
    try
    {
        std::ostringstream value_ss;
        value_ss << bw->value;  // integral values print without decimals ("5")
        const std::string expected = "; brim_width = " + value_ss.str();

        const std::string tmp_path = gcode_path + ".brimecho.tmp";
        std::ifstream in(gcode_path, std::ios::binary);
        if (!in.is_open())
        {
            BOOST_LOG_TRIVIAL(warning)
                << "brim echo restore: cannot open gcode " << gcode_path;
            return;
        }
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            BOOST_LOG_TRIVIAL(warning)
                << "brim echo restore: cannot create temp " << tmp_path;
            return;
        }

        // Own the temp file on every exit path (RAII): removed unless the
        // commit rename below moves it over the original. The error_code
        // overload keeps the destructor no-throw.
        struct TmpFileGuard
        {
            std::string path;
            bool committed = false;
            ~TmpFileGuard()
            {
                if (!committed)
                {
                    boost::system::error_code ec;
                    boost::filesystem::remove(path, ec);
                }
            }
        } tmp_guard{tmp_path, false};

        // Stream rewrite: the file can be hundreds of MB, so never buffer it
        // whole — copy line by line, replacing the single echo line. The
        // echo always sits between the "; CONFIG_BLOCK_START/END" markers
        // (file head for BBL printers, tail otherwise) and always carries the
        // zeroed slicing copy's value ("0"); anchoring on both keeps custom
        // gcode — written outside the block, verbatim from the untrusted
        // 3MF — from hijacking the rewrite.
        static const std::string kPrefix = "; brim_width = ";
        static const std::string kBlockStart = "; CONFIG_BLOCK_START";
        static const std::string kBlockEnd = "; CONFIG_BLOCK_END";
        bool replaced = false;
        bool in_config_block = false;
        std::string line;
        while (std::getline(in, line))
        {
            // Match on a copy without a trailing '\r' so a CRLF file still
            // hits (external MR !3 review F2); the ORIGINAL line is written
            // out, so an LF file stays byte-identical and a hypothetical
            // CRLF file keeps its EOLs everywhere except the one replaced
            // echo line. Engine-written gcode is always LF (GCode.cpp) —
            // this is robustness against post-processing, not an expected
            // input shape.
            std::string cmp = line;
            if (!cmp.empty() && cmp.back() == '\r')
                cmp.pop_back();
            if (cmp == kBlockStart)
                in_config_block = true;
            else if (cmp == kBlockEnd)
                in_config_block = false;

            if (!replaced && in_config_block && cmp.size() > kPrefix.size() &&
                cmp.compare(0, kPrefix.size(), kPrefix) == 0 &&
                cmp.substr(kPrefix.size()) == "0")
            {
                out << expected << "\n";
                replaced = true;
            }
            else
            {
                out << line << "\n";
            }
        }
        in.close();
        out.close();
        // Commit gate: a truncated copy must never replace the original.
        // Write failures (ENOSPC) surface at close()'s flush; a read failure
        // sets badbit and ends the getline loop early — both land here and
        // keep the intact original file.
        if (in.bad() || !out.good())
        {
            BOOST_LOG_TRIVIAL(error)
                << "brim echo restore: copy failed (I/O error), keeping original " << gcode_path;
            return;
        }
        if (!replaced)
        {
            BOOST_LOG_TRIVIAL(warning)
                << "brim echo restore: no zeroed '; brim_width = 0' line in the config block of "
                << gcode_path;
            return;
        }
        boost::system::error_code ec;
        boost::filesystem::rename(tmp_path, gcode_path, ec);
        if (ec)
        {
            BOOST_LOG_TRIVIAL(error)
                << "brim echo restore: rename failed (" << ec.message() << ") for " << gcode_path;
            return;
        }
        tmp_guard.committed = true;
        BOOST_LOG_TRIVIAL(info)
            << "brim_type=auto_brim: header echo brim_width restored to " << value_ss.str();
    }
    catch (const std::exception& e)
    {
        // Degrade, never escalate (see the note above the try): the echo
        // stays as sliced; the guard removes the partial temp copy.
        BOOST_LOG_TRIVIAL(warning)
            << "brim echo restore: failed (" << e.what() << "), echo left as-is in " << gcode_path;
    }
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
    // Per-slot flow type declaration (FilamentVolumeType raw ints). Snapshot
    // for the same reason as colours/types: this is the value get_config_idx
    // reads when resolving flow-variant keys AND the value the
    // "; filament_volume_type" gcode header echoes. No runtime writer touches
    // the key anywhere in the fork (GCode.cpp echo is read-only; PresetBundle
    // setters are desktop GUI-side), so snapshot == echo == the declaration
    // that actually drove slicing.
    if (print.config().has("filament_volume_type")) {
        const auto* fvt = print.config().option<ConfigOptionEnumsGeneric>("filament_volume_type");
        if (fvt) result.filament_volume_types = fvt->values;
    }
    // Per-slot support-material declaration. Same snapshot rationale as the
    // flow type: the runtime consumers (GCode / ToolOrdering / WipeTower)
    // read this value off print.config() and the "; filament_is_support"
    // gcode header echoes it, so this is the declaration that actually drove
    // slicing.
    if (print.config().has("filament_is_support")) {
        const auto* fis = print.config().option<ConfigOptionBools>("filament_is_support");
        if (fis) result.filament_is_supports = fis->values;
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
    // error_code overload: never throws — the throwing overload would escalate
    // a cleanup miss into an exception on a warning-level path.
    boost::system::error_code ec;
    boost::filesystem::remove(slice_result.gcode_path, ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "Plate " << (plate_id + 1)
                                   << ": failed to discard unusable G-code file ("
                                   << ec.message() << ")";
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
        for (auto& move : gcode_result.moves) {
            move.position += shift;
            // Arc interpolation points live in the same global grid space as
            // position (store_move_vertex adds the plate offset to both), so
            // they must shift too — otherwise arc moves on non-origin plates
            // pull the paths bbox into neighbouring plate cells and trip
            // TOOLPATH_OUTSIDE.
            for (auto& p : move.interpolation_points)
                p += shift;
        }
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

// Display-ready (name, plate-local 2D bbox) of this plate's instances,
// shared by the TOOLPATH_OUTSIDE and TOOL_HEIGHT_OUTSIDE attributions.
// Instance bboxes carry the global grid offset at the call site
// (run_build_volume_check restores it after its temporary shift), so
// translate by -plate_origin into the same plate-local space as the moves
// (gcode_result.moves were shifted in place by compute_toolpath_outside).
// Empty object names map to the shared "unnamed object" placeholder here so
// the pure collector stays display-ready in, display-ready out.
static std::vector<orca::OutsideAttributionSource> collect_attribution_sources(
    const Model& model, const std::set<int>& plate_instance_ids, const Vec2d& plate_origin)
{
    std::vector<orca::OutsideAttributionSource> sources;
    for (const ModelObject* obj : model.objects)
        for (const ModelInstance* inst : obj->instances)
            if (plate_instance_ids.count(static_cast<int>(inst->loaded_id)))
            {
                BoundingBoxf3 bb = obj->instance_bounding_box(*inst);
                bb.translate(Vec3d(-plate_origin.x(), -plate_origin.y(), 0.0));
                sources.push_back({obj->name.empty() ? "unnamed object" : obj->name,
                                   bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()});
            }
    return sources;
}

// Attribute out-of-bed extrusion moves to their sources so the
// TOOLPATH_OUTSIDE issue can name the culprit in object_name. Two channels:
//  - wipe tower moves carry the erWipeTower role (GCodeProcessor sets it on
//    the gcode ";TYPE:Prime tower" sections) -> synthetic name "wipe_tower"
//    (the tower is not a model object, so no real object name exists). The
//    tower's OWN brim is generated by WipeTower inside those sections, so it
//    carries the role too (gcode-body verified on Oathbreaker: tower
//    X<=283.0 vs bed 270.5 attributes "wipe_tower"; the separate ";TYPE:Brim"
//    sections are object brims and stay object-attributed);
//  - every other role (objects, supports, brims, skirts) is attributed to
//    the on-plate object instance whose plate-local bbox is NEAREST — brims
//    and skirts extrude beyond the object bbox, so pure containment would
//    miss them.
// Returns distinct sources in first-appearance (gcode) order, ","-joined;
// empty = no attribution possible (e.g. the bed_exclude_area trigger, whose
// offending points sit inside the bed polygon and only violate the exclude
// hull). Diagnostic only — the gate stays the plate-level toolpath_outside
// flag. The per-point tests use the raw bed polygon and a local epsilon
// (BuildVolume::BedEpsilon is private), so points inside the gate's
// forgiveness band may over-attribute to the adjacent near-boundary object.
// gcode_result.moves are already plate-local here (compute_toolpath_outside
// shifted them in place). The name resolution itself lives in
// OutsideAttribution.hpp (unit-tested there, shared with the height check).
static std::string attribute_outside_toolpath(const GCodeProcessorResult& gcode_result,
                                              const DynamicPrintConfig& config, const Model& model,
                                              const std::set<int>& plate_instance_ids, const Vec2d& plate_origin)
{
    const auto* printable_area_opt = config.option<ConfigOptionPoints>("printable_area");
    if (!printable_area_opt || printable_area_opt->values.empty() || plate_instance_ids.empty())
        return {};

    const BuildVolume build_volume(printable_area_opt->values, gcode_result.printable_height);
    const Polygon& bed = build_volume.polygon();
    const double max_z = gcode_result.printable_height; // 0 = unlimited (BuildVolume semantics)
    constexpr double k_bed_epsilon = 3. * EPSILON;      // mirrors BuildVolume::BedEpsilon (private)

    auto point_outside = [&](const Vec3f& p) {
        if (max_z > 0.0 && p.z() > max_z + k_bed_epsilon)
            return true;
        return !bed.contains(Point(scale_(p.x()), scale_(p.y())));
    };

    orca::OutsideAttributionCollector collector(
        collect_attribution_sources(model, plate_instance_ids, plate_origin));
    for (const auto& move : gcode_result.moves)
    {
        // Same validity filter as compute_toolpath_outside's bbox merge.
        if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom &&
            move.width != 0.0f && move.height != 0.0f)
        {
            const bool tower = move.extrusion_role == erWipeTower;
            if (point_outside(move.position))
                collector.add(move.position.x(), move.position.y(), tower);
            if (move.is_arc_move_with_interpolation_points())
                for (const auto& ip : move.interpolation_points)
                    if (point_outside(ip))
                        collector.add(ip.x(), ip.y(), tower);
        }
    }
    return collector.join();
}

// Attribute above-printable-height extrusion moves to their sources so the
// TOOL_HEIGHT_OUTSIDE issue can name the culprit in object_name — the same
// two channels (erWipeTower -> "wipe_tower", else nearest plate-local object
// bbox) and the same core (OutsideAttribution.hpp) as the bed-outside
// attribution above. Same h1 point policy: only EXTRUSION moves are fed to
// the collector, so a height violation carried purely by travel moves
// (z-hop) or custom G-code stays honestly unattributed (empty name) — those
// moves' XY positions have no reliable object ownership.
//
// Reachability note (verified 2026-11-11): three earlier gates normally
// absorb height violations — the pre-process build-volume check
// (BUILD_VOLUME_TOO_HIGH, model bbox, blocks the plate), Print::validate
// (top slicing plane vs printable_height + EPSILON=1e-4) and, on U1
// presets, retract_lift_below capping z-hop tops below the printable
// height. What realistically reaches THIS check: custom G-code markers
// (custom_gcode_per_layer.xml, erCustom, unattributed) and the
// validate(1e-4)-vs-here(1e-6) epsilon seam on extrusion tops.
//
// The gate stays the do_postprocessing max_z comparison; this attribution is
// diagnostic only. gcode_result.moves are already plate-local here
// (compute_toolpath_outside shifted them in place). plate_instance_ids is
// required non-empty by every caller path (the plate would have no object
// and no gcode moves at all otherwise), so no early return mirrors the
// outside variant's empty-ids guard.
static std::string attribute_outside_height(const GCodeProcessorResult& gcode_result,
                                            const Model& model, const std::set<int>& plate_instance_ids,
                                            const Vec2d& plate_origin)
{
    const float printable_height = gcode_result.printable_height;
    orca::OutsideAttributionCollector collector(
        collect_attribution_sources(model, plate_instance_ids, plate_origin));
    for (const auto& move : gcode_result.moves)
    {
        // Same validity filter as attribute_outside_toolpath: extrusion
        // moves only, no custom G-code, non-degenerate width/height.
        if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom &&
            move.width != 0.0f && move.height != 0.0f)
        {
            // Same threshold as the do_postprocessing gate (>= 1e-6 above
            // printable_height, float arithmetic promoted to double), not
            // the bed-outside k_bed_epsilon: this attribution must agree
            // with the gate it explains.
            const bool tower = move.extrusion_role == erWipeTower;
            if (move.position.z() - printable_height >= 1e-6)
                collector.add(move.position.x(), move.position.y(), tower);
            if (move.is_arc_move_with_interpolation_points())
                for (const auto& ip : move.interpolation_points)
                    if (ip.z() - printable_height >= 1e-6)
                        collector.add(ip.x(), ip.y(), tower);
        }
    }
    return collector.join();
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
        // Name the offending source(s) — "wipe_tower" or the object name(s)
        // — in object_name; empty when attribution is impossible (rare
        // bed_exclude_area trigger). Exactly ONE issue per plate stays a
        // contract invariant (error_code aggregation counts occurrences).
        const std::string sources = attribute_outside_toolpath(
            result.gcode_result, m_config, m_model, collect_plate_instance_ids(plate_id), plate_origin);
        const std::string log_msg = sources.empty()
                                        ? std::string("Toolpaths extend outside the printable area.")
                                        : "Toolpaths of \"" + sources + "\" extend outside the printable area.";
        const std::string issue_msg = sources.empty()
                                          ? std::string("Toolpaths extend outside the printable area. "
                                                        "The object exceeds the printer's build volume and cannot be printed safely.")
                                          : "Toolpaths of \"" + sources + "\" extend outside the printable area "
                                                                          "and cannot be printed safely.";
        log_plate_message("[Post-processing]", "ERROR", plate_id, log_msg);
        has_postprocess_error = true;
        result.issues.push_back(make_error(plate_id, "TOOLPATH_OUTSIDE", issue_msg, sources));
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
            // Name the offending source(s) — "wipe_tower" or the object
            // name(s) — in object_name; empty when attribution is impossible
            // (height carried only by travel/custom moves; see
            // attribute_outside_height). Exactly ONE issue per plate stays a
            // contract invariant (error_code aggregation counts occurrences).
            const std::string sources = attribute_outside_height(
                result.gcode_result, m_model, collect_plate_instance_ids(plate_id), plate_origin);
            const std::string log_msg = sources.empty()
                                            ? std::string("A G-code path goes beyond the max print height.")
                                            : "G-code paths of \"" + sources +
                                                  "\" go beyond the max print height.";
            const std::string issue_msg = sources.empty()
                                              ? std::string("A G-code path goes beyond the max print height. "
                                                            "The object may not print correctly.")
                                              : "G-code paths of \"" + sources +
                                                    "\" go beyond the max print height. "
                                                    "The object may not print correctly.";
            // Desktop shows this as ErrorType::SLICING_ERROR
            // (GLCanvas3D.cpp:9858), blocking the print dialog.
            log_plate_message("[Post-processing]", "ERROR", plate_id, log_msg);
            has_postprocess_error = true;
            Issue h = make_error(plate_id, "TOOL_HEIGHT_OUTSIDE", issue_msg, sources);
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
        mark_stage_failure(EXIT_POSTPROCESS_ERROR);
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
    // Shared with export_gcode's catch via clear_intermediate (smell C5).
    clear_intermediate(result);

    m_plate_results[plate_id] = result;
}

