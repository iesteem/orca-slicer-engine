#include "GeometryCheck.hpp"
#include "Types.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/MeshBoolean.hpp"

#include <boost/log/trivial.hpp>

namespace {

// Common suggestion strings shared across geometry checks.
// Extracted as constexpr to avoid duplication and allow single-point updates.
constexpr const char* SUGGESTION_REPAIR_NETFABB =
    "Repair the model with Netfabb (autodesk.com/netfabb), "
    "MeshMixer, Microsoft 3D Builder, or online services like formware.co. "
    "In CAD software: close all gaps and ensure the solid body is fully sealed.";

constexpr const char* SUGGESTION_DEGENERATE_FACES =
    "Use Edit Mode or a mesh repair tool to remove duplicate vertices "
    "and collapsed edges. In Blender: Select All > Mesh > Clean Up > "
    "Degenerate Dissolve.";

constexpr const char* SUGGESTION_SELF_INTERSECT =
    "Repair with Netfabb (autodesk.com/netfabb) or Blender 3D Print Toolbox. "
    "In CAD: check for overlapping extrusions, non-manifold Boolean operations, "
    "or imported mesh assemblies with intersecting parts.";

constexpr const char* SUGGESTION_MULTI_COMPONENT =
    "Use OrcaSlicer's split-to-parts (right-click > Split > To Parts) "
    "to separate components into individual objects. In CAD: export each "
    "part as a separate STL/STEP file for best results.";

constexpr const char* SUGGESTION_INVERTED_NORMALS =
    "In most slicers: right-click the object and select 'Fix model' or "
    "'Flip normals'. In CAD: ensure all faces point outward (solid body). "
    "In Blender: Edit mode > Select All > Mesh > Normals > Recalculate Outside.";

constexpr const char* SUGGESTION_EMPTY_MESH =
    "Remove the empty object from the project. "
    "Check the original CAD export — the part may have been empty "
    "or excluded from the export selection.";

constexpr const char* SUGGESTION_ZERO_VOLUME =
    "Verify the model is a solid 3D body, not a 2D surface or a single plane. "
    "In CAD: ensure the model has thickness in all three dimensions. "
    "Common cause: exported a sketch or surface instead of a solid body.";

// Helper: build a defect Issue for a given volume (delegates to factories in Types.hpp)
Issue make_issue(const std::string& level, int plate_id,
                 const std::string& object_name, const std::string& code,
                 const std::string& message,
                 const std::string& suggestion = "")
{
    if (level == "error")
        return make_error(plate_id, code, message, object_name, suggestion);
    else if (level == "tip")
        return make_tip(plate_id, code, message);
    else
        return make_warning(plate_id, code, message, object_name, suggestion);
}

void check_non_manifold(const Slic3r::ModelVolume& volume,
                        const std::string& obj_name, int plate_id,
                        std::vector<Issue>& out)
{
    const size_t total_edges = volume.mesh().its.indices.size() * 3;
    const size_t open_edges = Slic3r::its_num_open_edges(volume.mesh().its);
    if (open_edges > 0) {
        const double pct = total_edges > 0
            ? 100.0 * open_edges / total_edges : 100.0;
        std::array<char, 16> pct_buf;
        snprintf(pct_buf.data(), pct_buf.size(), "%.1f", pct);
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_NON_MANIFOLD",
            "Non-manifold mesh: " + std::to_string(open_edges) +
                " open edge(s) out of " + std::to_string(total_edges) +
                " total edges (" + pct_buf.data() + "%). "
                "The mesh is not watertight and will cause slicing artifacts.",
            SUGGESTION_REPAIR_NETFABB));
    }
}

void check_degenerate_faces(const Slic3r::ModelVolume& volume,
                            const std::string& obj_name, int plate_id,
                            std::vector<Issue>& out)
{
    const size_t total_faces = volume.mesh().its.indices.size();
    // FIXME: Copying the entire mesh to count degenerate faces is expensive
    // for large models. Replace with its_count_degenerate_faces() once
    // available in libslic3r, or iterate indices directly counting zero-area
    // triangles (vertices with equal indices).
    indexed_triangle_set its_copy = volume.mesh().its;
    const size_t removed = Slic3r::its_remove_degenerate_faces(its_copy);
    if (removed > 0) {
        const double pct = total_faces > 0
            ? 100.0 * removed / total_faces : 0;
        std::array<char, 16> pct_buf;
        snprintf(pct_buf.data(), pct_buf.size(), "%.1f", pct);
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_DEGENERATE",
            "Degenerate faces: " + std::to_string(removed) +
                " zero-area triangle(s) removed (" + pct_buf.data() +
                "% of " + std::to_string(total_faces) +
                " total faces). "
                "These cause zero-width extrusion paths and slicing gaps.",
            SUGGESTION_DEGENERATE_FACES));
    }
}

#ifdef _MSC_VER
// Standalone helper: wraps the CGAL call in SEH __try/__except.
// Must NOT use C++ try/catch and must NOT construct/destroy any C++
// objects — otherwise MSVC refuses to compile (C2712/C2713).
static bool cgal_self_intersect_safe(const Slic3r::TriangleMesh& mesh,
                                     bool* crashed)
{
    *crashed = false;
    __try {
        return Slic3r::MeshBoolean::cgal::does_self_intersect(mesh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return false;
    }
}
#endif

bool check_self_intersect(const Slic3r::ModelVolume& volume,
                          const std::string& obj_name, int plate_id,
                          std::vector<Issue>& out)
{
#ifdef _MSC_VER
    bool crashed = false;
    bool self_intersects = cgal_self_intersect_safe(volume.mesh(), &crashed);
    if (crashed) {
        BOOST_LOG_TRIVIAL(warning)
            << "GeometryCheck: CGAL self-intersect check crashed (SEH) for \""
            << obj_name << "\" — skipping";
    } else if (self_intersects) {
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_SELF_INTERSECT",
            "Self-intersecting mesh: faces penetrate each other. "
            "This will cause incorrect slicing results.",
            SUGGESTION_SELF_INTERSECT));
        return true;
    }
    return false;
#else
    try {
        if (Slic3r::MeshBoolean::cgal::does_self_intersect(volume.mesh())) {
            out.push_back(make_issue(
                "warning", plate_id, obj_name, "GEOM_SELF_INTERSECT",
                "Self-intersecting mesh: faces penetrate each other. "
                "This will cause incorrect slicing results.",
                SUGGESTION_SELF_INTERSECT));
            return true;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "GeometryCheck: CGAL self-intersect check failed for \""
            << obj_name << "\": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning)
            << "GeometryCheck: CGAL self-intersect check failed for \""
            << obj_name << "\" (unknown error)";
    }
    return false;
#endif
}

void check_multi_component(const Slic3r::ModelVolume& volume,
                           const std::string& obj_name, int plate_id,
                           std::vector<Issue>& out)
{
    const size_t patches = Slic3r::its_number_of_patches(
        volume.mesh().its);
    if (patches > 1) {
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_MULTI_COMPONENT",
            "Disconnected mesh: " + std::to_string(patches) +
                " separate component(s). Consider splitting the model for "
                "better orientation and support control per component.",
            SUGGESTION_MULTI_COMPONENT));
    }
}

void check_inverted_normals(const Slic3r::ModelVolume& volume,
                            const std::string& obj_name, int plate_id,
                            std::vector<Issue>& out)
{
    const double vol = Slic3r::its_volume(volume.mesh().its);
    if (vol < 0.0) {
        out.push_back(make_issue(
            "warning", plate_id, obj_name, "GEOM_INVERTED",
            "Inverted normals: mesh volume is negative (" +
                std::to_string(vol) +
                " mm³). Face winding (inside/outside) "
                "may be reversed, causing incorrect perimeter generation.",
            SUGGESTION_INVERTED_NORMALS));
    }
}

void check_empty(const Slic3r::ModelVolume& volume,
                 const std::string& obj_name, int plate_id,
                 std::vector<Issue>& out)
{
    if (volume.mesh().its.indices.empty()) {
        out.push_back(make_issue(
            "error", plate_id, obj_name, "GEOM_EMPTY",
            "Empty mesh: no triangles in model volume. "
            "The object has no geometry and cannot be sliced. "
            "This is an unrecoverable geometry error.",
            SUGGESTION_EMPTY_MESH));
    }
}

void check_zero_volume(const Slic3r::ModelVolume& volume,
                       const std::string& obj_name, int plate_id,
                       std::vector<Issue>& out)
{
    const double vol = Slic3r::its_volume(volume.mesh().its);
    // Threshold matching Model::removed_objects_with_zero_volume (1e-10)
    if (!volume.mesh().its.indices.empty() && std::abs(vol) < 1e-10) {
        out.push_back(make_issue(
            "error", plate_id, obj_name, "GEOM_ZERO_VOLUME",
            "Zero-volume mesh: the computed volume (" +
                std::to_string(vol) +
                " mm³) is effectively zero. "
                "This object cannot produce any extrusion. "
                "This is an unrecoverable geometry error.",
            SUGGESTION_ZERO_VOLUME));
    }
}

} // namespace

std::vector<Issue> run_geometry_checks(const Slic3r::Model& model, int plate_id)
{
    std::vector<Issue> issues;

    for (Slic3r::ModelObject* obj : model.objects) {
        if (obj == nullptr)
            continue;

        for (size_t vi = 0; vi < obj->volumes.size(); ++vi) {
            Slic3r::ModelVolume* vol = obj->volumes[vi];
            if (vol == nullptr || !vol->is_model_part())
                continue;

            std::string name = obj->name;
            if (obj->volumes.size() > 1)
                name += "[" + std::to_string(vi) + "]";

            check_empty(*vol, name, plate_id, issues);
            check_zero_volume(*vol, name, plate_id, issues);
            check_non_manifold(*vol, name, plate_id, issues);
            check_degenerate_faces(*vol, name, plate_id, issues);
            check_self_intersect(*vol, name, plate_id, issues);
            check_multi_component(*vol, name, plate_id, issues);
            check_inverted_normals(*vol, name, plate_id, issues);
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Geometry check completed: "
        << issues.size() << " defect(s) found";

    return issues;
}
