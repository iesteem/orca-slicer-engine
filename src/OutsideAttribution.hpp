#pragma once

// Pure attribution of offending gcode points to their sources, extracted from
// SliceEngine's attribute_outside_toolpath (88506f6) for unit testing and
// reuse by the TOOL_HEIGHT_OUTSIDE attribution. Only depends on the standard
// library — no libslic3r — so it compiles into the lightweight engine-tests
// target (same pattern as BuildVolumeClassify.hpp).
//
// The caller feeds ONLY points it has already judged offending (outside the
// bed polygon / above the printable height) via add(); this header does no
// violation math. Names arrive display-ready: the SliceEngine shell maps
// empty object names to "unnamed object" while collecting sources.

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace orca {

// Synthetic source name for wipe-tower moves (erWipeTower role): the tower
// is not a model object, so no real object name exists.
inline constexpr char kWipeTowerName[] = "wipe_tower";

// One on-plate object instance: display name + plate-local 2D AABB in mm.
// NSDMI on every scalar (R1): a bare `OutsideAttributionSource s;` must not
// read indeterminate doubles.
struct OutsideAttributionSource
{
    std::string name;
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
};

// Streams offending points in gcode order and attributes each to
//   - the wipe tower (is_wipe_tower), or
//   - the NEAREST source bbox by squared 2D AABB distance (0 inside). Nearest,
//     not containment: brims and skirts extrude beyond the object bbox, so
//     pure containment would miss them. Distance ties resolve to the
//     first-listed source (strict <).
// join() returns distinct names in first-appearance (gcode) order,
// ","-joined; empty = no attribution possible.
class OutsideAttributionCollector
{
public:
    explicit OutsideAttributionCollector(std::vector<OutsideAttributionSource> sources)
        : m_sources(std::move(sources)) {}

    void add(double x, double y, bool is_wipe_tower)
    {
        if (is_wipe_tower)
        {
            attribute(kWipeTowerName);
            return;
        }
        const OutsideAttributionSource* best = nullptr;
        double best_d2 = std::numeric_limits<double>::max();
        for (const auto& s : m_sources)
        {
            // Squared distance from (x, y) to the AABB: 0 inside, the axis
            // gap otherwise — the same formula the original SliceEngine code
            // used on BoundingBoxf3.
            const double dx = std::max({s.min_x - x, 0.0, x - s.max_x});
            const double dy = std::max({s.min_y - y, 0.0, y - s.max_y});
            const double d2 = dx * dx + dy * dy;
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best = &s;
            }
        }
        if (best)
            attribute(best->name);
    }

    std::string join() const
    {
        std::string out;
        for (const auto& name : m_names)
        {
            if (!out.empty())
                out += ",";
            out += name;
        }
        return out;
    }

private:
    void attribute(const std::string& name)
    {
        if (std::find(m_names.begin(), m_names.end(), name) == m_names.end())
            m_names.push_back(name);
    }

    std::vector<OutsideAttributionSource> m_sources;
    std::vector<std::string> m_names;
};

} // namespace orca
