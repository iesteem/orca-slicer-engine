#pragma once

// Pure build-volume violation classification, extracted from
// SliceEngine::push_build_volume_issues for unit testing. Only depends on
// Types.hpp (Issue/make_error) + standard library — no libslic3r — so it
// compiles into the lightweight engine-tests target.
//
// The caller (SliceEngine thin wrapper) is responsible for:
//   - converting the instance bbox to plate-local coordinates (origin already
//     subtracted) before passing the 6 raw doubles, and
//   - extracting printable_height + the 2D bed footprint from BuildVolume.

#include <cstdio>
#include <string>
#include <vector>

#include "Types.hpp"

namespace orca {

// Equal to libslic3r's BuildVolume::SceneEpsilon == EPSILON == 1e-4
// (libslic3r.h:52, BuildVolume.hpp:79). Reproduced here as a local constant so
// this header stays free of libslic3r.
inline constexpr double kSceneEpsilon = 1e-4;

// Classify which axis/axes a Partly_Outside instance violates. One bbox may
// yield multiple Issues (e.g. too high AND outside XY), returned in the order
// the original code emitted them: TOO_HIGH, OUTSIDE_XY.
//
// Note: sinking below the bed (min_z < 0) is intentionally NOT classified
// here. libslic3r's print-volume test treats a Below volume as neither inside
// nor outside (Model.cpp), so a pure sinking instance never reaches the
// Partly_Outside path that calls this function; and ensure_models_on_bed
// (SliceEngine.cpp) already handles every below-bed case via warnings
// (OBJECT_BELOW_BED_ADJUSTED / OBJECT_INTENTIONALLY_BELOW_BED), including the
// "only slice the above-bed portion" case. Keeping below-bed out of this
// classifier avoids a redundant error that could never block slicing on its
// own (the real blocker is always the accompanying TOO_HIGH / OUTSIDE_XY).
//
// bbox_* are plate-local mm; printable_height/bed_* describe the build volume.
std::vector<Issue> classify_build_volume_issues(
    int                plate_id,
    const std::string& object_name,
    double             bbox_min_x, double bbox_max_x,
    double             bbox_min_y, double bbox_max_y,
    double             bbox_min_z, double bbox_max_z,
    double             printable_height,
    double             bed_min_x,  double bed_max_x,
    double             bed_min_y,  double bed_max_y)
{
    const double eps = kSceneEpsilon;
    // bbox_min_z is retained in the signature for symmetry with bbox_max_z
    // and future extension, but is unused now that below-bed is handled by
    // ensure_models_on_bed. Silence -Wunused-parameter under -Wextra.
    (void)bbox_min_z;

    // TODO: snprintf("%.1f", -0.0) yields "-0.0"; the original code's comment
    // claims to "guard against -0.0" but does not. Behaviour is preserved
    // verbatim here (tests lock in "-0.0"); fixing the -0.0 is a separate task.
    const auto fmt_mm = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", v);
        return std::string(buf);
    };

    std::vector<Issue> out;

    // --- Z too high (top exceeds printable height) ---
    if (bbox_max_z > printable_height + eps)
    {
        Issue issue = make_error(
            plate_id, "BUILD_VOLUME_TOO_HIGH",
            "Object \"" + object_name + "\" top is at Z=" + fmt_mm(bbox_max_z) +
                "mm, exceeding the printable height of " + fmt_mm(printable_height) + "mm",
            object_name,
            "Lower the object's Z position, reduce its height, or scale it down so the top fits under the print height.");
        issue.z_height = bbox_max_z; // offending top Z
        out.push_back(std::move(issue));
    }

    // --- XY outside the bed footprint ---
    if (bbox_min_x < bed_min_x - eps || bbox_max_x > bed_max_x + eps ||
        bbox_min_y < bed_min_y - eps || bbox_max_y > bed_max_y + eps)
    {
        std::string detail;
        if (bbox_min_x < bed_min_x - eps)
            detail += "X=" + fmt_mm(bbox_min_x) + " < " + fmt_mm(bed_min_x) + "; ";
        if (bbox_max_x > bed_max_x + eps)
            detail += "X=" + fmt_mm(bbox_max_x) + " > " + fmt_mm(bed_max_x) + "; ";
        if (bbox_min_y < bed_min_y - eps)
            detail += "Y=" + fmt_mm(bbox_min_y) + " < " + fmt_mm(bed_min_y) + "; ";
        if (bbox_max_y > bed_max_y + eps)
            detail += "Y=" + fmt_mm(bbox_max_y) + " > " + fmt_mm(bed_max_y) + "; ";
        if (!detail.empty() && detail.back() == ' ')
            detail.pop_back(); // trim trailing "; "
        out.push_back(make_error(
            plate_id, "BUILD_VOLUME_OUTSIDE_XY",
            "Object \"" + object_name + "\" extends beyond the bed footprint (" + detail + ")",
            object_name,
            "Reposition the object within the bed area, or scale it down to fit."));
    }

    return out;
}

} // namespace orca
