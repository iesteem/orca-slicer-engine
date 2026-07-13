#pragma once

#include <set>

#include "EngineContext.hpp"
#include "Types.hpp"

struct PlateSliceResult;

namespace Slic3r {
    class Print;
}

/**
 * @file PlateProcessor.hpp
 * @brief Per-plate processing pipeline extracted from SliceEngine.
 *
 * PlateProcessor handles all per-plate operations: filtering instances,
 * build-volume checks, model application, validation, slicing, G-code
 * export, and post-processing.  It receives a reference to EngineContext
 * for shared mutable state (config, model, statistics, etc.) and does
 * not own any long-lived resources itself.
 *
 * Sub-stages are public for testability.
 */
class PlateProcessor {
public:
    explicit PlateProcessor(EngineContext& ctx);
    void process_plate(int plate_id);

    // Sub-stages (public for testability)
    int  filter_instances(int plate_id, std::set<int>& plate_instance_ids);
    bool run_build_volume_check(int plate_id, const std::set<int>& plate_instance_ids,
                                const Slic3r::Vec3d& origin);
    Slic3r::Vec3d setup_print_origin(int plate_id, double plate_width, double plate_depth);
    bool apply_model(int plate_id, Slic3r::Print& print, const Slic3r::Vec3d& origin);
    bool run_validation(int plate_id, Slic3r::Print& print);
    bool run_slicing(int plate_id, Slic3r::Print& print);
    bool export_gcode(int plate_id, Slic3r::Print& print, PlateSliceResult& result);
    void run_postprocessing(int plate_id, PlateSliceResult& result);
    void restore_baked_z_offsets();

private:
    void set_error_type(int code);

    EngineContext& m_ctx;
};
