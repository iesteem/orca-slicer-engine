/**
 * @file test_outside_attribution.cpp
 * @brief Unit tests for orca::OutsideAttributionCollector (extracted from
 *        SliceEngine's attribute_outside_toolpath).
 *
 * The collector is the attribution core shared by the TOOLPATH_OUTSIDE and
 * TOOL_HEIGHT_OUTSIDE issues: the caller feeds already-judged offending
 * points; the collector resolves each to "wipe_tower" or the nearest object
 * bbox and joins distinct names in first-appearance order. This file locks
 * that name-resolution core; the gcode-side integration (move filters,
 * per-point predicates) is covered by the [toolpath] e2e cases (Case 11/11b/
 * 11c) which drive this exact code through attribute_outside_toolpath.
 */

#include <catch_amalgamated.hpp>

#include <utility>

#include "OutsideAttribution.hpp"

using orca::OutsideAttributionCollector;
using orca::OutsideAttributionSource;

namespace {

OutsideAttributionSource box(std::string name,
                             double min_x, double min_y, double max_x, double max_y)
{
    return OutsideAttributionSource{std::move(name), min_x, min_y, max_x, max_y};
}

} // namespace

TEST_CASE("attribution: no offending points yields empty join", "[attribution]") {
    OutsideAttributionCollector c({box("A", 0, 0, 10, 10)});
    CHECK(c.join().empty());
}

TEST_CASE("attribution: tower points short-circuit to the synthetic name", "[attribution]") {
    OutsideAttributionCollector c({box("A", 0, 0, 10, 10)});
    c.add(5.0, 5.0, /*is_wipe_tower=*/true);
    CHECK(c.join() == orca::kWipeTowerName);
    CHECK(c.join() == "wipe_tower"); // lock the literal (JSON contract)
}

TEST_CASE("attribution: tower names even with no object sources", "[attribution]") {
    // The tower is not a model object; its attribution must not depend on
    // the source list being non-empty.
    OutsideAttributionCollector c({});
    c.add(200.0, 200.0, /*is_wipe_tower=*/true);
    CHECK(c.join() == "wipe_tower");
}

TEST_CASE("attribution: non-tower point with no sources is dropped", "[attribution]") {
    // Matches the original SliceEngine behaviour: best == nullptr -> the
    // point contributes no name (empty attribution = honest "unknown").
    OutsideAttributionCollector c({});
    c.add(5.0, 5.0, /*is_wipe_tower=*/false);
    CHECK(c.join().empty());
}

TEST_CASE("attribution: point inside a bbox resolves to that object (distance 0)", "[attribution]") {
    OutsideAttributionCollector c({box("cube", 100, 100, 120, 120)});
    c.add(110.0, 110.0, /*is_wipe_tower=*/false);
    CHECK(c.join() == "cube");
}

TEST_CASE("attribution: nearest bbox wins between two sources", "[attribution]") {
    OutsideAttributionCollector c({box("far", 0, 0, 10, 10), box("near", 40, 40, 50, 50)});
    // Equidistant gap axis-wise: 3mm from "near" (x=53 vs max 50), 33mm from "far".
    c.add(53.0, 45.0, /*is_wipe_tower=*/false);
    CHECK(c.join() == "near");
    // Now feed a point just 1mm outside the far box -> "far" wins instead.
    c.add(-1.0, 5.0, /*is_wipe_tower=*/false);
    CHECK(c.join() == "near,far");
}

TEST_CASE("attribution: distance ties resolve to the first-listed source", "[attribution]") {
    // Both boxes 1mm away from the point on the x axis; strict < keeps the
    // first-listed one. Locks the tie-break the original code had.
    OutsideAttributionCollector c({box("first", 0, 0, 10, 10), box("second", 20, 0, 30, 10)});
    c.add(15.0, 5.0, /*is_wipe_tower=*/false);
    CHECK(c.join() == "first");
}

TEST_CASE("attribution: far-from-everything points still attribute to the nearest", "[attribution]") {
    // Skirt/brim loops can sit well beyond every bbox; the nearest object is
    // still the best answer (gcode-body verified by Case 11b's skirt).
    OutsideAttributionCollector c({box("立方体", 241, 100, 268, 127)});
    c.add(274.6, 113.5, /*is_wipe_tower=*/false);
    CHECK(c.join() == "立方体");
}

TEST_CASE("attribution: names dedupe and keep first-appearance order", "[attribution]") {
    OutsideAttributionCollector c({box("A", 0, 0, 10, 10), box("B", 100, 0, 110, 10)});
    c.add(20.0, 5.0, /*is_wipe_tower=*/false);  // nearer A -> "A" first
    c.add(90.0, 5.0, /*is_wipe_tower=*/false);  // nearer B -> "B" appended after "A"
    c.add(20.0, 5.0, /*is_wipe_tower=*/false);  // duplicate A -> no new entry
    c.add(5.0, 5.0, /*is_wipe_tower=*/true);    // tower -> appended once
    c.add(6.0, 6.0, /*is_wipe_tower=*/true);    // duplicate tower -> no new entry
    CHECK(c.join() == "A,B,wipe_tower");
}

TEST_CASE("attribution: gcode order interleaves tower and objects", "[attribution]") {
    // Tower first, then an object point, then the tower again: order is
    // first-appearance, not category-grouped.
    OutsideAttributionCollector c({box("obj", 0, 0, 10, 10)});
    c.add(200.0, 200.0, /*is_wipe_tower=*/true);
    c.add(11.0, 5.0, /*is_wipe_tower=*/false);
    c.add(201.0, 201.0, /*is_wipe_tower=*/true);
    CHECK(c.join() == "wipe_tower,obj");
}
