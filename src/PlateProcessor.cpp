#include "PlateProcessor.hpp"
#include "SliceEngine.hpp"   // for EngineConfig + PlateSliceResult full defs
#include "GeometryCheck.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/core.hpp>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

constexpr int MAX_RETRIES = 3;

// Default plate dimensions, matching SliceEngine::DEFAULT_PLATE_WIDTH / _DEPTH
constexpr double DEFAULT_PLATE_WIDTH = 200.0;
constexpr double DEFAULT_PLATE_DEPTH = 200.0;

using namespace Slic3r;

namespace {

// 1/5, same as GUI's LOGICAL_PART_PLATE_GAP
constexpr double LOGICAL_PART_PLATE_GAP = 0.2;

// Named constant for magic number in post-processing
constexpr const char* BED_TEMP_WARNING_CODE = "1000C001";

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
// from print.process() are deterministic --- retrying with the same input
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

} // namespace

PlateProcessor::PlateProcessor(EngineContext& ctx)
    : m_ctx(ctx)
{
}

void PlateProcessor::set_error_type(int code)
{
    if (code > m_ctx.error_type)
        m_ctx.error_type = code;
}

// ============================================================================
// Process a single plate
// ============================================================================

void PlateProcessor::process_plate(int plate_id) {
    try {
    // --- Filter instances for this plate ---
    std::set<int> plate_instance_ids;
    int instances_on_plate = filter_instances(plate_id, plate_instance_ids);

    BOOST_LOG_TRIVIAL(info) << "Filtered model: " << instances_on_plate
        << " instances on plate " << (plate_id + 1);

    if (instances_on_plate == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Skipping empty plate " << (plate_id + 1);
        m_ctx.stats.issues.push_back(make_warning(plate_id, "EMPTY_PLATE",
            "Plate " + std::to_string(plate_id + 1) + " has no printable objects and was skipped."));
        return;
    }

    // Calculate plate dimensions and origin (done before build-volume check
    // so the check can translate instances into plate-local coordinates).
    double plate_width = DEFAULT_PLATE_WIDTH;
    double plate_depth = DEFAULT_PLATE_DEPTH;

    if (m_ctx.config.has("printable_area")) {
        auto printable_area_opt = m_ctx.config.option<ConfigOptionPoints>("printable_area");
        if (printable_area_opt && !printable_area_opt->values.empty()) {
            BoundingBoxf bbox;
            for (const Vec2d& pt : printable_area_opt->values)
                bbox.merge(pt);
            // Match desktop: PartPlateList::reset_size(int, int) truncates the
            // printable-area bounding-box width to int.  The desktop computes
            //   int(extended_bbox.max.x - extended_bbox.min.x - DefaultTipRadius)
            // where extended_bbox.min was already expanded by -DefaultTipRadius,
            // so the subtraction cancels --- the result is simply int(original_width).
            // We apply the same truncation directly on bbox.size().
            plate_width = static_cast<double>(static_cast<int>(bbox.size().x()));
            plate_depth = static_cast<double>(static_cast<int>(bbox.size().y()));
        }
    }

    Vec3d origin = setup_print_origin(plate_id, plate_width, plate_depth);

    // --- Build volume check (uses plate-local coordinates) ---
    if (!run_build_volume_check(plate_id, plate_instance_ids, origin))
        return;

    // Get BBL vendor flag once
    // TODO(PlateProcessor): PresetBundle not yet available via EngineContext.
    // Original code used:
    //   bool is_bbl = (m_presets_available && m_preset_bundle)
    //       ? m_preset_bundle->is_bbl_vendor() : false;
    // PresetManager/PresetBundle will provide this once integrated into the
    // EngineContext or passed as a separate dependency.
    bool is_bbl = false;

    // --- Slicing + Export (with retry for non-fatal exceptions) ---
    // Each attempt creates a fresh Print to avoid explicit dtor + placement-new
    // on the wipe-tower-disabled retry path.
    bool wipe_tower_disabled = false;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        // Check timeout before each attempt
        if (m_ctx.has_timeout && std::chrono::steady_clock::now() > m_ctx.timeout_deadline) {
            BOOST_LOG_TRIVIAL(error) << "Slicing timed out for plate " << (plate_id + 1);
            m_ctx.stats.issues.push_back(make_error(plate_id, "SLICING_TIMEOUT",
                "Slicing timed out. The model may be too complex. "
                "If you disagree, please submit an appeal for review."));
            m_ctx.any_error = true;
            set_error_type(EXIT_SLICING_ERROR);
            return;
        }

        if (attempt > 1) {
            BOOST_LOG_TRIVIAL(warning) << "Retry attempt " << attempt << "/" << MAX_RETRIES
                << " for plate " << (plate_id + 1);
            // Clear error issues from the previous failed attempt
            m_ctx.stats.issues.erase(
                std::remove_if(m_ctx.stats.issues.begin(), m_ctx.stats.issues.end(),
                    [&](const Issue& i) { return i.plate_id == plate_id && i.level == "error"; }),
                m_ctx.stats.issues.end());
            m_ctx.any_error = false;
            m_ctx.error_type = EXIT_OK;
        }

        // --- Create fresh Print for this attempt ---
        Print print;
        print.set_status_callback([&print, this](const PrintBase::SlicingStatus& s) {
            default_status_callback(s, &print, &m_ctx.cfg.cancel_file);
        });
        print.is_BBL_printer() = is_bbl;

        // --- Apply model ---
        if (!apply_model(plate_id, print, origin)) {
            restore_baked_z_offsets();
            return;
        }

        // --- Assign arrange_order ---
        {
            int order = 1;
            for (ModelObject* obj : m_ctx.model.objects)
                for (ModelInstance* inst : obj->instances)
                    inst->arrange_order = order++;
        }

        // --- Set global extruder params & speed table ---
        {
            int num_extruders = 0;
            if (m_ctx.config.has("filament_diameter")) {
                auto fd = m_ctx.config.option<ConfigOptionFloats>("filament_diameter");
                if (fd) num_extruders = static_cast<int>(fd->values.size());
            }
            Model::setExtruderParams(m_ctx.config, num_extruders);
            Model::setPrintSpeedTable(m_ctx.config, print.config());
        }

        // --- Validation ---
        if (!run_validation(plate_id, print))
            return;

        // Slicing
        if (!run_slicing(plate_id, print)) {
            // "No layers to export" is a non-retryable condition: objects on
            // this plate are below minimum layer height or have zero thickness.
            // Skip this plate gracefully and continue processing remaining
            // plates (matches desktop Orca GUI behavior).
            if (has_no_layers_on_plate(m_ctx.stats.issues, plate_id)) {
                BOOST_LOG_TRIVIAL(warning) << "Plate " << (plate_id + 1)
                    << ": no valid layers, skipping";
                restore_baked_z_offsets();
                return;
            }
            // Fatal slicing errors (SlicingError / SlicingErrors from geometry
            // issues) are deterministic --- retrying with unchanged input will
            // always fail identically.  Skip the plate instead of burning CPU
            // on 3 doomed retries.  (Matches desktop: no retry for these.)
            if (has_fatal_slicing_error_on_plate(m_ctx.stats.issues, plate_id)) {
                BOOST_LOG_TRIVIAL(warning) << "Plate " << (plate_id + 1)
                    << ": fatal slicing error, skipping (deterministic geometry failure)";
                restore_baked_z_offsets();
                return;
            }
            BOOST_LOG_TRIVIAL(warning) << "Slicing failed for plate " << (plate_id + 1)
                << " on attempt " << attempt;
            continue;
        }

        BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << (plate_id + 1);
        restore_baked_z_offsets();

        // Export G-code
        PlateSliceResult slice_result;
        if (!export_gcode(plate_id, print, slice_result)) {
            // Check if this is a wipe tower tool change mismatch.
            // On the next iteration a fresh Print is created with the updated config.
            bool is_wt_error = is_wipe_tower_error(slice_result);

            if (is_wt_error && !wipe_tower_disabled) {
                BOOST_LOG_TRIVIAL(warning) << "Wipe tower tool change mismatch on plate "
                    << (plate_id + 1) << " --- disabling wipe tower and re-slicing";
                wipe_tower_disabled = true;
                m_ctx.config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
                continue;
            }

            BOOST_LOG_TRIVIAL(warning) << "G-code export failed for plate " << (plate_id + 1)
                << " on attempt " << attempt;
            // Preserve export error details in global stats.
            // The retry-cleanup block above will clear previous errors
            // on the next iteration, so only the last attempt's details
            // will survive if all attempts fail.
            for (auto& iss : slice_result.issues)
                m_ctx.stats.issues.push_back(std::move(iss));
            slice_result.issues.clear();
            continue;
        }

        // Success
        run_postprocessing(plate_id, slice_result);

        // Free G-code visualization data that the cloud engine never uses.
        // GCodeProcessorResult::moves and lines_ends store every G-code move
        // vertex for the desktop GUI --- hundreds of MB per plate.  Retaining
        // them across plates causes std::bad_alloc on complex multi-plate
        // projects (e.g. Mochi Makes plate 3 with 10 instances after plates
        // 1+2 already consumed 500+ MB for their moves vectors).
        slice_result.gcode_result.moves.clear();
        slice_result.gcode_result.moves.shrink_to_fit();
        slice_result.gcode_result.lines_ends.clear();
        slice_result.gcode_result.lines_ends.shrink_to_fit();

        m_ctx.plate_results[plate_id] = slice_result;
        return;
    }

    // All retries exhausted
    restore_baked_z_offsets();
    BOOST_LOG_TRIVIAL(error) << "Slicing/export failed for plate " << (plate_id + 1)
        << " after " << MAX_RETRIES << " attempts";
    m_ctx.stats.error_message = "Failed to slice plate " + std::to_string(plate_id + 1)
        + " after " + std::to_string(MAX_RETRIES) + " attempts";
    m_ctx.any_error = true;
    // Preserve EXIT_SLICING_ERROR if set by run_slicing() (e.g. std::exception);
    // otherwise the failure came from export_gcode().
    if (m_ctx.error_type < EXIT_SLICING_ERROR)
        set_error_type(EXIT_EXPORT_ERROR);
    } catch (const std::exception& e) {
        restore_baked_z_offsets();
        BOOST_LOG_TRIVIAL(error) << "Unhandled exception processing plate "
            << (plate_id + 1) << ": " << e.what();
        m_ctx.any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        m_ctx.stats.issues.emplace_back(
            Issue{"error", plate_id, "", -1.0, "INTERNAL_ERROR",
                  std::string("Slicing failed for plate ")
                      + std::to_string(plate_id + 1)
                      + ": " + e.what(), ""});
    } catch (...) {
        restore_baked_z_offsets();
        BOOST_LOG_TRIVIAL(error) << "Unhandled non-standard exception "
            "processing plate " << (plate_id + 1);
        m_ctx.any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        m_ctx.stats.issues.emplace_back(
            Issue{"error", plate_id, "", -1.0, "INTERNAL_ERROR",
                  std::string("Slicing failed for plate ")
                      + std::to_string(plate_id + 1)
                      + " with unknown error", ""});
    }
}

// ============================================================================
// Per-plate sub-stages
// ============================================================================

int PlateProcessor::filter_instances(int plate_id, std::set<int>& plate_instance_ids) {
    for (const auto& pd : m_ctx.plate_data) {
        if (pd->plate_index == plate_id) {
            for (const auto& [object_id, inst_info] : pd->obj_inst_map) {
                plate_instance_ids.insert(inst_info.second);
            }
            break;
        }
    }

    int count = 0;
    for (ModelObject* obj : m_ctx.model.objects) {
        for (ModelInstance* inst : obj->instances) {
            bool on_plate = (plate_instance_ids.find(static_cast<int>(inst->loaded_id)) != plate_instance_ids.end());
            inst->printable = on_plate;
            inst->print_volume_state = on_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
            if (on_plate) ++count;
        }
    }
    return count;
}

bool PlateProcessor::run_build_volume_check(int plate_id, const std::set<int>& plate_instance_ids, const Vec3d& origin) {
    if (!(m_ctx.config.has("printable_area") && m_ctx.config.has("printable_height")))
        return true;

    auto printable_area_opt = m_ctx.config.option<ConfigOptionPoints>("printable_area");
    double printable_height = m_ctx.config.opt_float("printable_height");
    if (!printable_area_opt || printable_area_opt->values.empty() || printable_height <= 0)
        return true;

    // Translate build-volume check into this plate's local coordinate system.
    // Instances on different plates have grid-layout offsets in global space;
    // subtracting the plate origin gives their local position within the plate.
    BuildVolume build_volume(printable_area_opt->values, printable_height);

    // Temporarily shift on-plate instances into plate-local coordinates for the check,
    // then restore them afterwards.
    std::vector<std::pair<ModelInstance*, Vec3d>> shifted;
    for (ModelObject* obj : m_ctx.model.objects) {
        for (ModelInstance* inst : obj->instances) {
            if (!inst->printable) continue;
            int lid = static_cast<int>(inst->loaded_id);
            if (plate_instance_ids.find(lid) != plate_instance_ids.end()) {
                Vec3d global_offset = inst->get_offset();
                Vec3d local_offset = global_offset - origin;
                shifted.emplace_back(inst, global_offset);
                inst->set_offset(local_offset);
            }
        }
    }

    m_ctx.model.update_print_volume_state(build_volume);

    bool has_partly_outside = false;
    for (ModelObject* obj : m_ctx.model.objects) {
        for (ModelInstance* inst : obj->instances) {
            if (!inst->printable) continue;
            int lid = static_cast<int>(inst->loaded_id);
            if (plate_instance_ids.find(lid) == plate_instance_ids.end()) continue;

            if (inst->print_volume_state == ModelInstancePVS_Partly_Outside) {
                log_plate_message("[Pre-processing]", "ERROR", plate_id,
                    "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume.");
                has_partly_outside = true;
                m_ctx.stats.issues.push_back(make_error(plate_id, "BUILD_VOLUME_PARTLY_OUTSIDE",
                    "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume",
                    obj->name));
            } else if (inst->print_volume_state == ModelInstancePVS_Fully_Outside) {
                m_ctx.stats.issues.push_back(make_warning(plate_id, "BUILD_VOLUME_FULLY_OUTSIDE",
                    "Object \"" + obj->name + "\" is completely outside the build volume and will not be printed",
                    obj->name));
            }
        }
    }

    // Snapmaker: SpiralLiftNearBoundary warning (matches desktop 3DScene.cpp:1105-1122)
    {
        bool spiral_lift_active = false;
        if (m_ctx.config.has("z_hop_types")) {
            auto zht_opt = m_ctx.config.option<ConfigOptionEnumsGeneric>("z_hop_types");
            if (zht_opt) {
                for (int v : zht_opt->values) {
                    if (v == static_cast<int>(ZHopType::zhtSpiral) ||
                        v == static_cast<int>(ZHopType::zhtAuto)) {
                        spiral_lift_active = true;
                        break;
                    }
                }
            }
        }
        if (spiral_lift_active && build_volume.type() == BuildVolume_Type::Rectangle) {
            constexpr double SPIRAL_LIFT_SAFETY_MARGIN = 3.5; // mm
            const BoundingBoxf3& bed_bb = build_volume.bounding_volume();
            std::set<std::string> warned_objects;
            for (ModelObject* obj : m_ctx.model.objects) {
                if (!obj->printable) continue;
                for (size_t idx = 0; idx < obj->instances.size(); ++idx) {
                    ModelInstance* inst = obj->instances[idx];
                    if (!inst->printable) continue;
                    int lid = static_cast<int>(inst->loaded_id);
                    if (plate_instance_ids.find(lid) == plate_instance_ids.end()) continue;
                    if (inst->print_volume_state != ModelInstancePVS_Inside) continue;
                    BoundingBoxf3 bb = obj->instance_bounding_box(idx);
                    double dist_left   = std::abs(bb.min.x() - bed_bb.min.x());
                    double dist_right  = std::abs(bed_bb.max.x() - bb.max.x());
                    double dist_bottom = std::abs(bb.min.y() - bed_bb.min.y());
                    double dist_top    = std::abs(bed_bb.max.y() - bb.max.y());
                    double min_dist    = std::min({dist_left, dist_right, dist_bottom, dist_top});
                    if (min_dist < SPIRAL_LIFT_SAFETY_MARGIN) {
                        if (warned_objects.insert(obj->name).second) {
                            m_ctx.stats.issues.push_back(make_warning(plate_id,
                                "SPIRAL_LIFT_NEAR_BOUNDARY",
                                "Model too close to bed boundary. "
                                "Disable spiral lifting or keep at least 3.5mm gap to avoid collision.",
                                obj->name));
                        }
                    }
                }
            }
        }
    }

    // Restore global offsets for on-plate instances and update printable state
    for (auto& [inst, global_offset] : shifted) {
        inst->set_offset(global_offset);
    }
    // Mark off-plate instances as not printable for this plate
    for (ModelObject* obj : m_ctx.model.objects) {
        for (ModelInstance* inst : obj->instances) {
            int lid = static_cast<int>(inst->loaded_id);
            bool on_plate = (plate_instance_ids.find(lid) != plate_instance_ids.end());
            inst->printable = on_plate;
            if (on_plate)
                inst->print_volume_state = ModelInstancePVS_Inside;
        }
    }

    if (has_partly_outside) {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " has objects outside build volume, skipping";
        m_ctx.any_error = true;
        set_error_type(EXIT_VALIDATION_ERROR);
        return false;
    }
    return true;
}

Vec3d PlateProcessor::setup_print_origin(int plate_id, double plate_width, double plate_depth) {
    // Compute plate origin using the same grid layout formula as the desktop GUI
    // (PartPlate::update_plate_layout_arrange). Each plate occupies a cell in a
    // row-major grid with LOGICAL_PART_PLATE_GAP spacing between plates.
    int total_plates = static_cast<int>(m_ctx.plate_data.size());
    int cols = compute_column_count(total_plates);
    int row = plate_id / cols;
    int col = plate_id % cols;

    double origin_x = col * (plate_width  * (1.0 + LOGICAL_PART_PLATE_GAP));
    double origin_y = -row * (plate_depth * (1.0 + LOGICAL_PART_PLATE_GAP));

    Vec3d origin(origin_x, origin_y, 0.0);
    return origin;
}

bool PlateProcessor::apply_model(int plate_id, Print& print, const Vec3d& origin) {
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
    //   1. vol->get_extruders()  --- per-volume assignment + MMU painting
    //   2. plates_custom_gcodes  --- AMS per-layer ToolChange entries
    //   3. single_extruder_multi_material --- non-Bambu single-extruder multi-material
    // Only trim when ALL sources agree the model is single-extruder.
    //
    // Work on a per-plate copy so extruder-count trimming does not leak
    // into subsequent plates (m_config is shared across the pipeline).
    // Build full config (system defaults + printer + filament + project)
    // before per-plate trimming and overrides.

    // TODO(PlateProcessor): build_full_print_config() is still on SliceEngine
    // and accesses PresetBundle members not yet available via EngineContext.
    // The original call was:
    //   DynamicPrintConfig merged_config = build_full_print_config();
    // Once PresetManager is created with access to system presets, this should
    // be replaced with a call that layers:
    //   1. FullPrintConfig::defaults()
    //   2. System process preset (from print_settings_id)
    //   3. System printer preset (from printer_settings_id)
    //   4. System filament config (from filament_settings_id)
    //   5. m_ctx.config (project config from 3MF)
    // For now, apply a minimal fallback that at least provides defaults:
    DynamicPrintConfig merged_config;
    merged_config.apply(FullPrintConfig::defaults());
    merged_config.apply(m_ctx.config);

    {
        std::set<int> used_extruders;
        for (ModelObject* obj : m_ctx.model.objects) {
            for (ModelInstance* inst : obj->instances) {
                if (!inst->is_printable())
                    continue;
                for (ModelVolume* vol : obj->volumes) {
                    if (!vol->is_model_part())
                        continue;
                    for (int eid : vol->get_extruders())
                        used_extruders.insert(eid);
                }
            }
        }

        int num_filaments = 0;
        if (m_ctx.config.has("filament_diameter")) {
            auto fd = m_ctx.config.option<ConfigOptionFloats>("filament_diameter");
            if (fd) num_filaments = static_cast<int>(fd->values.size());
        }

        // If volumes suggest single extruder, also check plate-level ToolChange
        // custom G-code (AMS per-layer filament switching).
        if (used_extruders.size() <= 1 && num_filaments > 1) {
            auto it = m_ctx.model.plates_custom_gcodes.find(plate_id);
            if (it != m_ctx.model.plates_custom_gcodes.end()) {
                for (const auto& item : it->second.gcodes) {
                    if (item.type == CustomGCode::Type::ToolChange && item.extruder > 0)
                        used_extruders.insert(item.extruder);
                }
            }
        }

        // If still single, also check the single_extruder_multi_material config flag
        // (used by non-Bambu printers for single-nozzle multi-filament).
        if (used_extruders.size() <= 1 && num_filaments > 1) {
            auto semm = m_ctx.config.option<ConfigOptionBool>("single_extruder_multi_material");
            if (semm && semm->value) {
                // Model genuinely uses multiple filaments through one extruder.
                // Insert sentinel values to prevent trimming.
                used_extruders.insert(1);
                used_extruders.insert(2);
            }
        }

        if (used_extruders.size() <= 1 && num_filaments > 1) {
            // Single extruder model with multiple filaments configured.
            // Disable the prime/wipe tower to prevent tool change errors,
            // but keep filament arrays intact so model volumes referencing
            // non-zero extruders still map correctly.
            BOOST_LOG_TRIVIAL(info) << "Disabling prime tower for single-extruder plate "
                << (plate_id + 1) << " (used extruders: "
                << (used_extruders.empty() ? "none" : std::to_string(*used_extruders.begin()))
                << ", filaments configured: " << num_filaments << ")";
            merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
        } else if (used_extruders.size() <= 1) {
            BOOST_LOG_TRIVIAL(info) << "Disabling prime tower (single extruder model)";
            merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
        }
    }

    // Apply per-plate config overrides (curr_bed_type, print_sequence, spiral_mode, etc.)
    for (const auto& pd : m_ctx.plate_data) {
        if (pd->plate_index == plate_id && !pd->config.empty()) {
            merged_config.apply(pd->config);
            break;
        }
    }

    // Z-baking: move instance Z translation into volume Z offset.
    // Print::apply() (PrintApply.cpp:155-157) zeroes the instance Z
    // translation in the PrintObject trafo, so the mesh Z position must
    // come entirely from the volume transform.  For instances with Z_scale
    // in the transformation matrix, the Z translation must be divided by
    // Z_scale before adding to the volume, because Print::apply() preserves
    // Z_scale and the combined Z = Z_scale * (volume_z + mesh_z).
    //
    // Invalidate ModelObject bounding-box caches before Z-baking.
    // ModelObject::update_min_max_z() caches its result via m_min_max_z_valid,
    // which is only cleared by invalidate_bounding_box().  If a previous plate
    // left this cache valid after restoring baked offsets, the next plate would
    // compute layer heights from stale Z extents, causing "No layers were
    // detected" errors for plates whose instance Z offset differs.
    for (ModelObject* obj : m_ctx.model.objects)
        if (obj != nullptr)
            obj->invalidate_bounding_box();

    m_ctx.baked_instance_z.clear();
    for (ModelObject* obj : m_ctx.model.objects) {
        if (obj == nullptr) continue;
        std::vector<ModelInstance*> printable_instances;
        for (ModelInstance* inst : obj->instances) {
            if (inst != nullptr && inst->is_printable())
                printable_instances.push_back(inst);
        }
        if (printable_instances.size() != 1) continue;
        ModelInstance* inst = printable_instances.front();
        const Vec3d inst_offset = inst->get_offset();
        if (std::abs(inst_offset.z()) < 1e-9) continue;
        // Get Z_scale from the instance transformation matrix.
        // The 4x4 matrix (Transform3d) is stored column-major in Eigen.
        // Element (2,2) is the Z scaling factor for the volume's unit-Z offset.
        //
        // When the object is rotated ~90 degrees, element (2,2) can be near-zero
        // (e.g. -2e-06) while the Z-column norm is large (~6.05). Dividing by a
        // near-zero value produces a massive z_adjustment (~-7.8 million mm) that
        // corrupts the volume offset and cascades into extreme G-code coordinates
        // and absurd print time estimates.
        //
        // Detect this by comparing element (2,2) against the Z-column norm.
        // If the contribution of Z to world Z is negligible, skip Z-baking.
        Eigen::Vector3d z_col = inst->get_transformation().get_matrix().matrix().block<3,1>(0, 2);
        double z_col_norm = z_col.norm();
        double z_scale = inst->get_transformation().get_matrix()(2, 2);
        if (std::abs(z_col_norm) < 1e-9) continue;
        if (std::abs(z_scale) / z_col_norm < 1e-6) continue;
        if (std::abs(z_scale) < 1e-9) continue;
        double z_adjustment = inst_offset.z() / z_scale;
        BakedInstanceZ baked;
        baked.inst = inst;
        baked.inst_offset = inst_offset;
        for (ModelVolume* vol : obj->volumes) {
            if (vol == nullptr) continue;
            baked.volume_offsets.emplace_back(vol, vol->get_offset());
            vol->set_offset(vol->get_offset() + z_adjustment * Vec3d::UnitZ());
        }
        inst->set_offset(Vec3d(inst_offset.x(), inst_offset.y(), 0.0));
        m_ctx.baked_instance_z.push_back(std::move(baked));
    }

    print.apply(m_ctx.model, merged_config);

    if (print.num_object_instances() == 0) {
        m_ctx.stats.issues.push_back(make_warning(plate_id, "NO_PRINTABLE_OBJECTS",
            "No printable objects on this plate after apply"));
        return false;
    }

    return true;
}

void PlateProcessor::restore_baked_z_offsets()
{
    for (auto it = m_ctx.baked_instance_z.rbegin(); it != m_ctx.baked_instance_z.rend(); ++it) {
        if (it->inst == nullptr)
            continue;
        it->inst->set_offset(it->inst_offset);
        for (auto vol_it = it->volume_offsets.begin();
             vol_it != it->volume_offsets.end(); ++vol_it) {
            if (vol_it->first != nullptr)
                vol_it->first->set_offset(vol_it->second);
        }
    }
    // Invalidate ModelObject bounding-box caches after restoring offsets.
    // This is a defensive measure: even if a future code path bypasses
    // apply_model()'s explicit invalidate_bounding_box(), the stale cached
    // Z extents won't carry over to the next plate.
    for (const auto& baked : m_ctx.baked_instance_z) {
        if (baked.inst != nullptr) {
            Slic3r::ModelObject* obj = baked.inst->get_object();
            if (obj != nullptr)
                obj->invalidate_bounding_box();
        }
    }
    m_ctx.baked_instance_z.clear();
}

bool PlateProcessor::run_validation(int plate_id, Print& print) {
    StringObjectException warning;
    StringObjectException err = print.validate(&warning, nullptr, nullptr);

    if (!warning.string.empty()) {
        auto [obj_name, opt_hint] = format_exception_context(warning);
        std::cerr << "[WARNING] Plate " << plate_id << ": " << warning.string << obj_name << opt_hint << std::endl;
        std::string wcode;
        switch (warning.type) {
            case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE:  wcode = "PRINT_VALIDATE_WARNING_FILAMENT_BED_MISMATCH"; break;
            case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP:     wcode = "PRINT_VALIDATE_WARNING_FILAMENT_TEMP_MISMATCH"; break;
            case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT:  wcode = "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_SEQ"; break;
            case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT:wcode = "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_LAYER"; break;
            case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT:    wcode = "PRINT_VALIDATE_WARNING_LAYER_HEIGHT_LIMIT"; break;
            default:                                          wcode = "PRINT_VALIDATE_WARNING"; break;
        }
        m_ctx.stats.issues.push_back(make_warning(plate_id, wcode,
            warning.string + opt_hint, obj_name));
    }

    if (!err.string.empty()) {
        auto [obj_name, opt_hint] = format_exception_context(err);
        // All non-empty err.string types (including STRING_EXCEPT_NOT_DEFINED)
        // are treated as blocking errors, matching the desktop GUI behavior
        // (Plater.cpp:12290-12339 sets UPDATE_BACKGROUND_PROCESS_INVALID).
        {
            std::cerr << "[ERROR] Plate " << plate_id << ": " << err.string << obj_name << opt_hint << std::endl;
            std::string ecode;
            switch (err.type) {
                case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE:  ecode = "PRINT_VALIDATE_FILAMENT_BED_MISMATCH"; break;
                case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP:     ecode = "PRINT_VALIDATE_FILAMENT_TEMP_MISMATCH"; break;
                case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT:  ecode = "PRINT_VALIDATE_OBJECT_COLLISION_SEQ"; break;
                case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT:ecode = "PRINT_VALIDATE_OBJECT_COLLISION_LAYER"; break;
                case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT:    ecode = "PRINT_VALIDATE_LAYER_HEIGHT_LIMIT"; break;
                default:                                          ecode = "PRINT_VALIDATE_ERROR"; break;
            }
            m_ctx.stats.issues.push_back(make_error(plate_id, ecode,
                err.string + opt_hint, obj_name));
            m_ctx.any_error = true;
            set_error_type(EXIT_VALIDATION_ERROR);
            return false;
        }
    }

    return true;
}

bool PlateProcessor::run_slicing(int plate_id, Print& print) {
    BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << plate_id << "...";

    try {
        ScopedLogLevel quiet_slice(boost::log::trivial::warning);
        print.process();
    }
    catch (const SlicingErrors& exs) {
        for (const auto& ex : exs.errors_) {
            std::string msg = ex.what();
            BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << msg;
            std::cerr << "[ERROR] Plate " << plate_id << ": slicing failed: " << msg << std::endl;
            m_ctx.stats.issues.push_back(make_error(plate_id, "SLICING_ERROR",
                "Slicing failed for this plate: " + msg));
        }
        m_ctx.any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const SlicingError& ex) {
        std::string msg = ex.what();
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << msg;

        // Non-fatal slicing errors: GUI downgrades these to warnings and
        // still produces gcode. Match GUI behavior.
        bool is_non_fatal = (msg.find("empty initial layer") != std::string::npos ||
                             msg.find("No layers were detected") != std::string::npos);

        if (is_non_fatal) {
            // "No layers were detected" is treated as non-fatal in the GUI to
            // allow multi-plate prints where other plates have valid geometry.
            // However, proceeding with G-code export when the current plate has
            // zero layers causes a SIGSEGV in GCode::_do_export. Skip export
            // and report the plate as failed.
            if (msg.find("No layers were detected") != std::string::npos) {
                BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id
                    << ": no layers to slice, skipping export";
                m_ctx.stats.issues.push_back(make_error(plate_id, "SLICING_ERROR",
                    "No layers to export: all objects have zero thickness "
                    "or are below the minimum layer height."));
                return false;
            }

            // Other non-fatal errors (e.g., empty initial layer) --- proceed
            BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id
                << ": non-fatal slicing issue (matching GUI behavior), "
                << "proceeding with export: " << msg;
            m_ctx.stats.issues.push_back(make_warning(plate_id, "SLICING_WARNING",
                "Slicing completed with warnings: " + msg));
            return true;
        }

        std::cerr << "[ERROR] Plate " << plate_id << ": slicing failed" << std::endl;
        m_ctx.stats.issues.push_back(make_error(plate_id, "SLICING_ERROR",
            "Slicing failed for this plate. The model may contain geometry that cannot be sliced."));
        m_ctx.any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const CanceledException&) {
        std::cerr << "[ERROR] Plate " << plate_id << ": Slicing was cancelled." << std::endl;
        m_ctx.stats.issues.push_back(make_error(plate_id, "SLICING_CANCELLED",
            "Slicing was cancelled"));
        m_ctx.any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing exception: " << e.what();
        std::cerr << "[ERROR] Plate " << plate_id << ": slicing failed due to an internal error" << std::endl;
        m_ctx.stats.issues.push_back(make_error(plate_id, "SLICING_EXCEPTION",
            "Slicing failed due to an internal error. Please try again."));
        m_ctx.any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }

    return true;
}

bool PlateProcessor::export_gcode(int plate_id, Print& print, PlateSliceResult& result) {
    std::string gcode_output;
    if (m_ctx.cfg.format == OutputFormat::GCODE_3MF || !m_ctx.cfg.single_plate) {
        gcode_output = m_ctx.cfg.temp_dir + "/plate_" + std::to_string(plate_id) + ".gcode";
        m_ctx.temp_files.push_back(gcode_output);
    } else {
        gcode_output = m_ctx.output_path;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code for plate " << plate_id << "...";

    try {
        GCodeProcessor::s_IsBBLPrinter = print.is_BBL_printer();
        std::string exported;
        {
            ScopedLogLevel quiet_export(boost::log::trivial::warning);
            exported = print.export_gcode(gcode_output, &result.gcode_result, nullptr);
        }
        result.gcode_path = exported;

        if (exported.empty() && !gcode_output.empty()) {
            BOOST_LOG_TRIVIAL(error) << "G-code export returned empty path for plate " << plate_id;
            result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_EMPTY",
                "G-code export produced no output file. The model may contain geometry that cannot be sliced into printable toolpaths."));
            m_ctx.any_error = true;
            set_error_type(EXIT_EXPORT_ERROR);
            return false;
        }

        // Post-processing scripts are disabled in cloud mode to prevent
        // remote code execution via user-uploaded 3MF files.
        // run_post_process_scripts(result.gcode_path, print.full_print_config());

        // Collect PrintBase warnings (EmptyGcodeLayers, G-code overlap, etc.)
        // CRITICAL warnings (e.g. EmptyGcodeLayers) are mapped to "warning" level
        // (not "error") because G-code was generated successfully.  The desktop GUI
        // shows them as modal dialogs for UX prominence, not as blocking failures.
        // Using a distinct code preserves severity for API consumers while keeping
        // the exit code, success flags, and statistics collection correct.
        for (int step = 0; step < static_cast<int>(PrintStep::psCount); ++step) {
            auto wstate = print.step_state_with_warnings(static_cast<PrintStep>(step));
            for (const auto& w : wstate.warnings) {
                if (!w.current) continue;
                bool is_critical = (w.level == PrintStateBase::WarningLevel::CRITICAL);
                if (is_critical)
                    result.issues.push_back(make_warning(plate_id, "PRINT_WARNING_CRITICAL", w.message));
                else
                    result.issues.push_back(make_warning(plate_id, "PRINT_WARNING", w.message));
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
    catch (const StringObjectException& e) {
        auto [obj_name, opt_hint] = format_exception_context(e);
        std::string detail = e.string + opt_hint;
        BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << plate_id
            << ": " << detail << obj_name;
        result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_CONFIG_ERROR",
            detail, obj_name));
        m_ctx.any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        return false;
    }
    catch (const SlicingErrors& exs) {
        for (const auto& ex : exs.errors_) {
            BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " export error: " << ex.what();
            result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_SLICING_ERROR",
                ex.what()));
        }
        m_ctx.any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        return false;
    }
    catch (const SlicingError& ex) {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " export error: " << ex.what();
        result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_SLICING_ERROR",
            ex.what()));
        m_ctx.any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        return false;
    }
    catch (const CanceledException&) {
        BOOST_LOG_TRIVIAL(error) << "G-code export cancelled for plate " << plate_id;
        result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_CANCELLED",
            "G-code export was cancelled"));
        m_ctx.any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        return false;
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << plate_id << ": " << e.what();
        result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_ERROR",
            std::string("G-code export failed: ") + e.what()));
        m_ctx.any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        return false;
    }
}

void PlateProcessor::run_postprocessing(int plate_id, PlateSliceResult& result) {
    bool has_postprocess_error = false;
    bool has_postprocess_warning = false;

    // Toolpaths outside print volume. Desktop uses this only for
    // GCode preview visualization, not to block gcode output.
    if (result.gcode_result.toolpath_outside) {
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Some toolpaths are outside the printable area.");
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "TOOLPATH_OUTSIDE",
            "Some toolpaths are outside the printable area. "
            "The object may not print correctly."));
    }

    // Tool height outside check.
    // Desktop already checks height in Print::validate() (Print.cpp:1629-1643).
    // No need for a duplicate post-processing check.

    // Toolpath conflict detection.
    // Desktop allows printing with conflicts (PartPlate.hpp:433:
    // "gcode conflict can also print"). Match that behavior --- warning only.
    if (result.gcode_result.conflict_result.has_value()) {
        const auto& cr = result.gcode_result.conflict_result.value();
        std::string obj1 = cr._obj1 ? cr._objName1 : "Wipe Tower";
        std::string obj2 = cr._obj2 ? cr._objName2 : "Wipe Tower";
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Toolpath conflict detected between \"" + obj1 + "\" and \"" + obj2
            + "\" at Z=" + std::to_string(cr._height) + "mm.");
        has_postprocess_warning = true;
        Issue conflict = make_warning(plate_id, "TOOLPATH_CONFLICT",
            "Toolpath conflict detected between \"" + obj1 + "\" and \"" + obj2
            + "\" at Z=" + std::to_string(cr._height) + "mm",
            obj1 + " vs " + obj2);
        conflict.z_height = cr._height;
        result.issues.push_back(conflict);
    }

    // Bed/filament compatibility
    if (!result.gcode_result.bed_match_result.match) {
        const auto& bm = result.gcode_result.bed_match_result;
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "BED_FILAMENT_MISMATCH",
            "Filament " + std::to_string(bm.extruder_id + 1)
            + " is not compatible with bed type \"" + bm.bed_type_name + "\""));
    }

    // Timelapse warnings
    if (result.gcode_result.timelapse_warning_code & 1) {
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "TIMELAPSE_SPIRAL_VASE",
            "Timelapse is not supported in spiral vase mode on this printer"));
    }
    if ((result.gcode_result.timelapse_warning_code >> 1) & 1) {
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "TIMELAPSE_BY_OBJECT",
            "Timelapse is not supported with by-object print sequence on this printer"));
    }

    // Slice warnings
    for (const auto& w : result.gcode_result.warnings) {
        if (w.error_code == BED_TEMP_WARNING_CODE) continue;
        // Level 2 = "severe warning" in GUI --- still produces gcode.
        // Only level 3+ is a true error that blocks export.
        if (w.level >= 3) {
            log_plate_message("[Post-processing]", "ERROR", plate_id,
                w.msg + " (code: " + w.error_code + ")");
            has_postprocess_error = true;
            result.issues.push_back(make_error(plate_id, w.error_code,
                w.msg + " (code: " + w.error_code + ")"));
        } else if (w.level >= 1) {
            has_postprocess_warning = true;
            result.issues.push_back(make_warning(plate_id, w.error_code,
                w.msg + " (code: " + w.error_code + ")"));
        } else {
            result.issues.push_back(make_tip(plate_id, w.error_code, w.msg));
        }
    }

    result.has_postprocess_warning = has_postprocess_warning;
    if (has_postprocess_warning)
        m_ctx.any_postprocess_warning = true;
    if (has_postprocess_error) {
        m_ctx.any_error = true;
        set_error_type(EXIT_VALIDATION_ERROR);
    }
}
