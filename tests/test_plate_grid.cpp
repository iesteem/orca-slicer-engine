/**
 * @file test_plate_grid.cpp
 * @brief Unit tests for the pure plate-grid-layout helpers in PlateGrid.hpp.
 *
 * Covers:
 *   - compute_column_count()  — regression guard after move from Utils.hpp
 *   - plate_grid_cell()       — row/col decomposition
 *   - compute_plate_origin()  — global grid origin (X/Y)
 *
 * These are pure functions with no libslic3r dependency, so they compile into
 * the lightweight engine-tests target. Floating-point assertions use WithinAbs
 * per project convention (never raw ==).
 */

#include <catch_amalgamated.hpp>
// WithinAbs is provided by the amalgamation itself; the catch2/matchers/*
// sub-headers do not exist in the single-header distribution.

#include "PlateGrid.hpp"

using orca::compute_column_count;
using orca::compute_plate_origin;
using orca::plate_grid_cell;
using Catch::Matchers::WithinAbs;

// ============================================================================
// compute_column_count — regression guard (moved here from Utils.hpp).
// Behaviour must be identical to the previously-tested Utils.hpp version.
// ============================================================================

TEST_CASE("compute_column_count: degenerate inputs clamp to 1", "[plategrid][cols]") {
    // Guards the count<=0 clamp + the negative→NaN→int UB fix.
    CHECK(compute_column_count(0) == 1);
    CHECK(compute_column_count(-1) == 1);
    CHECK(compute_column_count(-100) == 1);
}

TEST_CASE("compute_column_count: single item", "[plategrid][cols]") {
    CHECK(compute_column_count(1) == 1);
}

TEST_CASE("compute_column_count: perfect squares", "[plategrid][cols]") {
    CHECK(compute_column_count(4) == 2);
    CHECK(compute_column_count(9) == 3);
    CHECK(compute_column_count(16) == 4);
}

TEST_CASE("compute_column_count: non-perfect squares", "[plategrid][cols]") {
    CHECK(compute_column_count(2) == 2);  // sqrt2≈1.41 → round 1, value>round → 2
    CHECK(compute_column_count(3) == 2);  // sqrt3≈1.73 → round 2, value<=round → 2
    CHECK(compute_column_count(5) == 3);  // sqrt5≈2.24 → round 2, value>round → 3
}

// ============================================================================
// plate_grid_cell — row/col decomposition in a row-major grid
// ============================================================================

TEST_CASE("plate_grid_cell: 4-plate grid (2 cols)", "[plategrid][cell]") {
    // cols = compute_column_count(4) = 2
    CHECK(plate_grid_cell(0, 4).row == 0);  CHECK(plate_grid_cell(0, 4).col == 0);
    CHECK(plate_grid_cell(1, 4).row == 0);  CHECK(plate_grid_cell(1, 4).col == 1);
    CHECK(plate_grid_cell(2, 4).row == 1);  CHECK(plate_grid_cell(2, 4).col == 0);
    CHECK(plate_grid_cell(3, 4).row == 1);  CHECK(plate_grid_cell(3, 4).col == 1);
}

TEST_CASE("plate_grid_cell: 9-plate grid (3 cols)", "[plategrid][cell]") {
    CHECK(plate_grid_cell(4, 9).row == 1);  CHECK(plate_grid_cell(4, 9).col == 1);
    CHECK(plate_grid_cell(8, 9).row == 2);  CHECK(plate_grid_cell(8, 9).col == 2);
}

// ============================================================================
// compute_plate_origin — global origin, X grows right, Y grows down (negative)
// ============================================================================

TEST_CASE("compute_plate_origin: single plate is at origin", "[plategrid][origin]") {
    auto o = compute_plate_origin(0, 1, 200.0, 200.0);
    REQUIRE_THAT(o.x, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(o.y, WithinAbs(0.0, 1e-6));
}

TEST_CASE("compute_plate_origin: 4-plate grid uses 1.2x spacing", "[plategrid][origin]") {
    // plate 1 → col 1 → x = 1 * 200 * 1.2 = 240
    auto o1 = compute_plate_origin(1, 4, 200.0, 200.0);
    REQUIRE_THAT(o1.x, WithinAbs(240.0, 1e-6));
    REQUIRE_THAT(o1.y, WithinAbs(0.0, 1e-6));

    // plate 2 → row 1 → y = -(1 * 200 * 1.2) = -240
    auto o2 = compute_plate_origin(2, 4, 200.0, 200.0);
    REQUIRE_THAT(o2.x, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(o2.y, WithinAbs(-240.0, 1e-6));

    // plate 3 → row 1 col 1 → (240, -240)
    auto o3 = compute_plate_origin(3, 4, 200.0, 200.0);
    REQUIRE_THAT(o3.x, WithinAbs(240.0, 1e-6));
    REQUIRE_THAT(o3.y, WithinAbs(-240.0, 1e-6));
}

TEST_CASE("compute_plate_origin: 9-plate grid second row", "[plategrid][origin]") {
    // plate 4 → row 1 col 1 (cols=3) → (240, -240)
    auto o = compute_plate_origin(4, 9, 200.0, 200.0);
    REQUIRE_THAT(o.x, WithinAbs(240.0, 1e-6));
    REQUIRE_THAT(o.y, WithinAbs(-240.0, 1e-6));
}

TEST_CASE("compute_plate_origin: non-square plate size", "[plategrid][origin]") {
    // plate 1, cols=2, width=256 depth=192 → x = 1*256*1.2 = 307.2
    auto o = compute_plate_origin(1, 2, 256.0, 192.0);
    REQUIRE_THAT(o.x, WithinAbs(307.2, 1e-4));
    REQUIRE_THAT(o.y, WithinAbs(0.0, 1e-6));
}

TEST_CASE("compute_plate_origin: degenerate size yields zero origin", "[plategrid][origin]") {
    auto o = compute_plate_origin(3, 4, 0.0, 0.0);
    REQUIRE_THAT(o.x, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(o.y, WithinAbs(0.0, 1e-6));
}
