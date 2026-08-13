/**
 * @file test_build_volume_classify.cpp
 * @brief Unit tests for orca::classify_build_volume_issues (extracted from
 *        SliceEngine::push_build_volume_issues).
 *
 * Covers each violation axis independently, multi-axis cases (order/count),
 * the eps boundary, the OUTSIDE_XY detail string, and the locked-in -0.0
 * formatting behaviour.
 */

#include <catch_amalgamated.hpp>
// WithinAbs is provided by the amalgamation itself.

#include "BuildVolumeClassify.hpp"

using orca::classify_build_volume_issues;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {
// Standard 200x200 bed, 200mm tall. eps = 1e-4.
struct Bed { double min_x, max_x, min_y, max_y, height; };
constexpr Bed kBed{0.0, 200.0, 0.0, 200.0, 200.0};

// bbox fully inside -> no issues.
std::vector<Issue> classify_box(Bed b,
                                double min_x, double max_x,
                                double min_y, double max_y,
                                double min_z, double max_z,
                                int plate = 1, const std::string& name = "Obj")
{
    return classify_build_volume_issues(plate, name,
        min_x, max_x, min_y, max_y, min_z, max_z,
        b.height, b.min_x, b.max_x, b.min_y, b.max_y);
}
} // namespace

TEST_CASE("build volume: fully-inside box yields no issues", "[buildvolume]") {
    auto v = classify_box(kBed, 50, 150, 50, 150, 0, 50);
    CHECK(v.empty());
}

TEST_CASE("build volume: too high", "[buildvolume]") {
    auto v = classify_box(kBed, 50, 150, 50, 150, 0, 250);
    REQUIRE(v.size() == 1);
    CHECK(v[0].code == "BUILD_VOLUME_TOO_HIGH");
    CHECK(v[0].level == IssueLevel::error);
    REQUIRE_THAT(v[0].z_height, WithinAbs(250.0, 1e-9));
    CHECK(v[0].message.find("Obj") != std::string::npos);
}

TEST_CASE("build volume: below bed (XY in-range) yields no issue — handled by bake_instance_z_into_mesh", "[buildvolume]") {
    // min_z<0 but XY in-range and not too high: pure sinking is intentionally
    // NOT classified here (libslic3r never flags it Partly_Outside, and
    // bake_instance_z_into_mesh owns all below-bed feedback). Expect zero issues.
    auto v = classify_box(kBed, 50, 150, 50, 150, -10, 50);
    REQUIRE(v.size() == 0);
}

TEST_CASE("build volume: outside XY (min X under)", "[buildvolume]") {
    auto v = classify_box(kBed, -5, 150, 50, 150, 0, 50);
    REQUIRE(v.size() == 1);
    CHECK(v[0].code == "BUILD_VOLUME_OUTSIDE_XY");
    REQUIRE_THAT(v[0].z_height, WithinAbs(-1.0, 1e-9)); // OUTSIDE_XY keeps default -1
    REQUIRE_THAT(v[0].message, ContainsSubstring("X=-5.0 < 0.0"));
}

TEST_CASE("build volume: outside XY detail trims trailing separator", "[buildvolume]") {
    // Only min X violates -> detail should NOT end with "; "
    auto v = classify_box(kBed, -5, 150, 50, 150, 0, 50);
    REQUIRE(v.size() == 1);
    // Message contains the detail inside parens; it must not contain a trailing "; "
    auto pos = v[0].message.find('(');
    REQUIRE(pos != std::string::npos);
    std::string detail = v[0].message.substr(pos + 1);
    auto close = detail.find(')');
    detail = detail.substr(0, close);
    CHECK(detail.find("; ") == std::string::npos); // single direction, no separator
}

TEST_CASE("build volume: outside XY multiple directions in detail", "[buildvolume]") {
    // min X and max Y both violate -> "X=... < ...; Y=... > ..."
    auto v = classify_box(kBed, -5, 150, 50, 250, 0, 50);
    REQUIRE(v.size() == 1);
    REQUIRE_THAT(v[0].message, ContainsSubstring("X=-5.0 < 0.0"));
    REQUIRE_THAT(v[0].message, ContainsSubstring("Y=250.0 > 200.0"));
}

TEST_CASE("build volume: too high AND outside XY -> ordered, two issues", "[buildvolume]") {
    // TOO_HIGH fires first, then OUTSIDE_XY.
    auto v = classify_box(kBed, -5, 150, 50, 150, 0, 250);
    REQUIRE(v.size() == 2);
    CHECK(v[0].code == "BUILD_VOLUME_TOO_HIGH");
    CHECK(v[1].code == "BUILD_VOLUME_OUTSIDE_XY");
    REQUIRE_THAT(v[0].z_height, WithinAbs(250.0, 1e-9));
    REQUIRE_THAT(v[1].z_height, WithinAbs(-1.0, 1e-9));
}

TEST_CASE("build volume: too high (with min_z<0) -> only TOO_HIGH, below-bed handled elsewhere", "[buildvolume]") {
    // max_z exceeds printable_height AND min_z<0. Only TOO_HIGH fires here;
    // the below-bed aspect is reported by bake_instance_z_into_mesh, not this
    // classifier.
    auto v = classify_box(kBed, 50, 150, 50, 150, -10, 250);
    REQUIRE(v.size() == 1);
    CHECK(v[0].code == "BUILD_VOLUME_TOO_HIGH");
}

TEST_CASE("build volume: eps boundary (exactly printable_height + eps) does not fire", "[buildvolume]") {
    // bbox_max_z == printable_height + eps -> strict > is false -> no TOO_HIGH.
    const double eps = orca::kSceneEpsilon;
    auto v = classify_box(kBed, 50, 150, 50, 150, 0, kBed.height + eps);
    CHECK(v.empty());
}

TEST_CASE("build volume: eps boundary just past fires", "[buildvolume]") {
    const double eps = orca::kSceneEpsilon;
    auto v = classify_box(kBed, 50, 150, 50, 150, 0, kBed.height + eps + 0.001);
    REQUIRE(v.size() == 1);
    CHECK(v[0].code == "BUILD_VOLUME_TOO_HIGH");
}

TEST_CASE("build volume: -0.0 formatting locked in (regression guard)", "[buildvolume]") {
    // fmt_mm historically renders a negative value that rounds to zero as
    // "-0.0" (the original "guard against -0.0" comment was inaccurate). Lock
    // the current behaviour until a dedicated fix. Use -0.04: it violates
    // (min_y < bed_min_y - eps) and rounds to -0.0 under "%.1f".
    auto v = classify_build_volume_issues(1, "Obj",
        50, 150, -0.04, 150, 0, 50,
        kBed.height, kBed.min_x, kBed.max_x, kBed.min_y, kBed.max_y);
    REQUIRE(v.size() == 1);
    CHECK(v[0].code == "BUILD_VOLUME_OUTSIDE_XY");
    REQUIRE_THAT(v[0].message, ContainsSubstring("Y=-0.0 < 0.0"));
}
