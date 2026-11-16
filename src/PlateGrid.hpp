#pragma once

// Pure plate-grid-layout logic, extracted from SliceEngine for unit testing.
// This header is deliberately dependency-light: only <cmath>. It must NOT
// include anything that pulls in libslic3r, so it can be compiled into the
// lightweight engine-tests target (which does not link libslic3r).

#include <cmath>

namespace orca {

// Inter-plate spacing in the grid layout, expressed as a fraction of plate
// size. 1/5, same as the desktop GUI's LOGICAL_PART_PLATE_GAP. inline so the
// definition can live in a header shared across translation units.
inline constexpr double LOGICAL_PART_PLATE_GAP = 0.2;

// Compute column count for plate grid layout (matches GUI's PartPlate.hpp logic).
// Moved here from Utils.hpp so PlateGrid.hpp is self-contained.
inline int compute_column_count(int count)
{
    // Degenerate inputs (zero/negative plate counts) clamp to a single column.
    // This also avoids sqrt() of a negative → NaN, whose conversion to int is UB.
    if (count <= 0)
        return 1;
    float value = sqrt(static_cast<float>(count));
    float round_value = round(value);
    return (value > round_value) ? (round_value + 1) : round_value;
}

// Grid cell position for a plate in a row-major layout.
struct PlateCell
{
    int row;
    int col;
};

inline PlateCell plate_grid_cell(int plate_id, int total_plates)
{
    int cols = compute_column_count(total_plates);
    return { plate_id / cols, plate_id % cols };
}

// Plate origin in global space, X/Y only (Z is always 0).
struct PlateOrigin
{
    double x;
    double y;
};

// Compute the global coordinate origin for a plate in the row-major grid
// layout (mirrors the desktop GUI PartPlate::update_plate_layout_arrange
// formula). Each plate occupies a cell of size (plate_width, plate_depth) with
// LOGICAL_PART_PLATE_GAP spacing between plates; Y grows downward (negative).
//
// Pure: depends only on its arguments. The caller (SliceEngine::setup_print_origin
// thin wrapper) is responsible for short-circuiting when plate_width /
// plate_depth are unavailable (e.g. printable_area missing); this function
// always produces a deterministic value for any inputs, including degenerate
// sizes (0,0 → origin (0,0)).
inline PlateOrigin compute_plate_origin(int plate_id, int total_plates,
                                        double plate_width, double plate_depth)
{
    PlateCell cell = plate_grid_cell(plate_id, total_plates);
    return {
        cell.col * plate_width * (1.0 + LOGICAL_PART_PLATE_GAP),
        -cell.row * plate_depth * (1.0 + LOGICAL_PART_PLATE_GAP),
    };
}

// Plate footprint rectangle in global XY space: the plate-local bed extent
// [0,W]x[0,D] translated by the grid origin, inflated by eps on every side.
// Desktop parity: PartPlate::get_build_volume (GUI/PartPlate.hpp) inflates the
// plate box by BuildVolume::SceneEpsilon before intersect_instance() tests
// it against an instance bounding box. Z is deliberately excluded — every
// engine plate shares the same z-range, so XY alone discriminates membership.
struct PlateBox2D
{
    double xmin;
    double ymin;
    double xmax;
    double ymax;
};

// Pure companion of compute_plate_origin for P-23 geometric membership
// rebuild (see SliceEngine::rebuild_plate_membership). The Y span is
// [origin.y, origin.y + D]: the grid origin is the plate's upper edge and
// rows grow downward (negative Y), consistent with setup_print_origin's
// global-to-local translation by plain subtraction.
inline PlateBox2D compute_plate_box(int plate_id, int total_plates,
                                    double plate_width, double plate_depth,
                                    double eps)
{
    PlateOrigin o = compute_plate_origin(plate_id, total_plates, plate_width, plate_depth);
    return { o.x - eps, o.y - eps, o.x + plate_width + eps, o.y + plate_depth + eps };
}

} // namespace orca
