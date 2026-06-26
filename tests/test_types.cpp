/**
 * @file test_types.cpp
 * @brief Unit tests for the Issue factory functions in Issue.hpp.
 *
 * Tests cover:
 *   - make_error()   — verifies level, plate_id, code, z_height
 *   - make_warning() — verifies level and plate_id
 *   - make_tip()     — verifies level, empty suggestion, empty object_name
 */

#include <catch2/catch_all.hpp>

#include "Issue.hpp"

// ============================================================================
// make_error
// ============================================================================

TEST_CASE("make_error: basic fields", "[types][issue]") {
    Issue issue = make_error(1, "TEST_ERR", "A test error occurred");
    CHECK(issue.level    == "error");
    CHECK(issue.plate_id == 1);
    CHECK(issue.code     == "TEST_ERR");
    CHECK(issue.message  == "A test error occurred");
    CHECK(issue.z_height == -1.0);
    CHECK(issue.object_name.empty());
    CHECK(issue.suggestion.empty());
}

TEST_CASE("make_error: with object name and suggestion", "[types][issue]") {
    Issue issue = make_error(3, "OVERHANG",
                             "Overhang angle exceeds limit",
                             "MyObject",
                             "Add supports or reduce overhang angle");
    CHECK(issue.level       == "error");
    CHECK(issue.plate_id    == 3);
    CHECK(issue.code        == "OVERHANG");
    CHECK(issue.object_name == "MyObject");
    CHECK(issue.suggestion  == "Add supports or reduce overhang angle");
    CHECK(issue.z_height    == -1.0);
}

TEST_CASE("make_error: global plate_id is -1", "[types][issue]") {
    Issue issue = make_error(-1, "GLOBAL_ERR", "Global error");
    CHECK(issue.plate_id == -1);
}

// ============================================================================
// make_warning
// ============================================================================

TEST_CASE("make_warning: basic fields", "[types][issue]") {
    Issue issue = make_warning(2, "TEMP_WARN", "Temperature may be too high");
    CHECK(issue.level    == "warning");
    CHECK(issue.plate_id == 2);
    CHECK(issue.code     == "TEMP_WARN");
    CHECK(issue.z_height == -1.0);
}

TEST_CASE("make_warning: with optional fields", "[types][issue]") {
    Issue issue = make_warning(0, "SPEED", "Print speed exceeds recommended",
                               "Base", "Reduce speed");
    CHECK(issue.level       == "warning");
    CHECK(issue.object_name == "Base");
    CHECK(issue.suggestion  == "Reduce speed");
}

// ============================================================================
// make_tip
// ============================================================================

TEST_CASE("make_tip: basic fields", "[types][issue]") {
    Issue issue = make_tip(0, "TIP_001", "Consider using a brim");
    CHECK(issue.level    == "tip");
    CHECK(issue.plate_id == 0);
    CHECK(issue.code     == "TIP_001");
    CHECK(issue.message  == "Consider using a brim");
    CHECK(issue.z_height == -1.0);
}

TEST_CASE("make_tip: suggestion and object_name are always empty", "[types][issue]") {
    // make_tip has no optional parameters, so these should always be defaulted.
    Issue issue = make_tip(1, "ORIENT", "Rotate model for better quality");
    CHECK(issue.suggestion.empty());
    CHECK(issue.object_name.empty());
}
