#pragma once

#include <vector>

namespace Slic3r
{
class Model;
class ModelObject;
class ModelVolume;
} // namespace Slic3r

struct Issue;

/**
 * Run all geometry quality checks on the model.
 *
 * Checks every model part for: empty mesh, zero volume, non-manifold edges,
 * degenerate faces, self-intersections, multiple disconnected components,
 * and inverted normals. Performed before slicing so defects can be reported
 * in the JSON output.
 *
 * @param model    The model to check (all objects and their volumes).
 * @param plate_id Plate identifier for issue tagging (-1 = no plate context).
 * @return Vector of Issue objects describing each defect found.
 */
std::vector<Issue> run_geometry_checks(const Slic3r::Model& model,
                                        int plate_id = -1);
