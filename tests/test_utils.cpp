/**
 * @file test_utils.cpp
 * @brief Unit tests for the utility functions in Utils.hpp / Utils.cpp.
 *
 * Tests cover:
 *   - format_time_hhmmss()   — edge cases: zero, positive, negative, NaN, inf
 *   - compute_column_count() — grid column computation for plate layouts
 *   - generate_output_path() — single-plate, custom-base, and all-plates paths
 */

#include <catch2/catch_all.hpp>

#include <cmath>
#include <limits>

#include "Utils.hpp"

// ============================================================================
// format_time_hhmmss
// ============================================================================

TEST_CASE("format_time_hhmmss: zero seconds", "[utils][time]") {
    CHECK(format_time_hhmmss(0.0f) == "00:00:00");
}

TEST_CASE("format_time_hhmmss: typical duration", "[utils][time]") {
    // 3661 seconds = 1 hour, 1 minute, 1 second
    CHECK(format_time_hhmmss(3661.0f) == "01:01:01");
}

TEST_CASE("format_time_hhmmss: large value", "[utils][time]") {
    // 86399 seconds = 23h 59m 59s (one second before midnight)
    CHECK(format_time_hhmmss(86399.0f) == "23:59:59");
}

TEST_CASE("format_time_hhmmss: fractional seconds are truncated", "[utils][time]") {
    CHECK(format_time_hhmmss(59.999f) == "00:00:59");
}

TEST_CASE("format_time_hhmmss: negative input returns zero", "[utils][time]") {
    CHECK(format_time_hhmmss(-30.0f) == "00:00:00");
    CHECK(format_time_hhmmss(-1.0f) == "00:00:00");
}

TEST_CASE("format_time_hhmmss: NaN input returns zero", "[utils][time]") {
    float nan_val = std::numeric_limits<float>::quiet_NaN();
    CHECK(format_time_hhmmss(nan_val) == "00:00:00");
}

TEST_CASE("format_time_hhmmss: infinity input returns zero", "[utils][time]") {
    float inf_val = std::numeric_limits<float>::infinity();
    CHECK(format_time_hhmmss(inf_val) == "00:00:00");
    CHECK(format_time_hhmmss(-inf_val) == "00:00:00");
}

// ============================================================================
// compute_column_count
// ============================================================================

TEST_CASE("compute_column_count: zero or negative items", "[utils][grid]") {
    // The function clamps to 1 for degenerate inputs.
    CHECK(compute_column_count(0) == 1);
    CHECK(compute_column_count(-1) == 1);
    CHECK(compute_column_count(-100) == 1);
}

TEST_CASE("compute_column_count: single item", "[utils][grid]") {
    CHECK(compute_column_count(1) == 1);
}

TEST_CASE("compute_column_count: perfect squares", "[utils][grid]") {
    // sqrt(4) = 2.0 → round(2.0) = 2 → no extra column needed
    CHECK(compute_column_count(4) == 2);
    // sqrt(9) = 3.0 → round(3.0) = 3
    CHECK(compute_column_count(9) == 3);
    // sqrt(16) = 4.0 → round(4.0) = 4
    CHECK(compute_column_count(16) == 4);
}

TEST_CASE("compute_column_count: non-perfect squares", "[utils][grid]") {
    // sqrt(2) ≈ 1.41 → round(1.41) = 1, value > round → 1 + 1 = 2
    CHECK(compute_column_count(2) == 2);
    // sqrt(3) ≈ 1.73 → round(1.73) = 2, value <= round → 2
    CHECK(compute_column_count(3) == 2);
    // sqrt(5) ≈ 2.24 → round(2.24) = 2, value > round → 2 + 1 = 3
    CHECK(compute_column_count(5) == 3);
    // sqrt(6) ≈ 2.45 → round(2.45) = 2, value > round → 2 + 1 = 3
    CHECK(compute_column_count(6) == 3);
    // sqrt(7) ≈ 2.65 → round(2.65) = 3, value <= round → 3
    CHECK(compute_column_count(7) == 3);
}

// ============================================================================
// generate_output_path
// ============================================================================

TEST_CASE("generate_output_path: single plate auto-derived path", "[utils][path]") {
    // Use a non-existent path to avoid collision-suffix interference.
    std::string input = "/tmp/orca-test-nonexistent-dir/model.3mf";
    std::string path  = generate_output_path(input, "", 1, OutputFormat::GCODE_3MF, true);
    // Single-plate auto-derived path: <dirname>/<stem>-p<id>.gcode.3mf
    CHECK(path.find("-p1.gcode.3mf") != std::string::npos);
    CHECK(path.find("model") != std::string::npos);
}

TEST_CASE("generate_output_path: single plate gcode format", "[utils][path]") {
    std::string input = "/tmp/orca-test-nonexistent-dir/model.3mf";
    std::string path  = generate_output_path(input, "", 1, OutputFormat::GCODE, true);
    CHECK(path.find("-p1.gcode") != std::string::npos);
    CHECK(path.find(".gcode.3mf") == std::string::npos);
}

TEST_CASE("generate_output_path: custom output base", "[utils][path]") {
    std::string input  = "/tmp/orca-test-nonexistent-dir/model.3mf";
    std::string base   = "/tmp/orca-test-nonexistent-dir/my_output";
    std::string path   = generate_output_path(input, base, 1, OutputFormat::GCODE, true);
    CHECK(path.find("my_output.gcode") != std::string::npos);
    CHECK(path.find("model") == std::string::npos);
}

TEST_CASE("generate_output_path: all plates always uses gcode.3mf", "[utils][path]") {
    std::string input = "/tmp/orca-test-nonexistent-dir/model.3mf";
    std::string path  = generate_output_path(input, "", 0, OutputFormat::GCODE, false);
    // All-plates mode ignores format param and always produces .gcode.3mf
    CHECK(path.find(".gcode.3mf") != std::string::npos);
}

TEST_CASE("generate_output_path: all plates with custom base", "[utils][path]") {
    std::string input  = "/tmp/orca-test-nonexistent-dir/model.3mf";
    std::string base   = "/tmp/orca-test-nonexistent-dir/all_result";
    std::string path   = generate_output_path(input, base, 0, OutputFormat::GCODE_3MF, false);
    CHECK(path.find("all_result.gcode.3mf") != std::string::npos);
}
