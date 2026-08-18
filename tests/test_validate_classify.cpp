/**
 * @file test_validate_classify.cpp
 * @brief Unit tests for orca::classify_validate_exception (extracted from
 *        SliceEngine::emit_validate_warning / emit_validate_error).
 *
 * Covers the full type x path classification matrix, the NOT_DEFINED organic
 * substring branch, the ORGANIC escalation (warning path), and the
 * continue_slicing contract (error path only).
 */

#include <catch_amalgamated.hpp>

#include "ValidateClassify.hpp"

using orca::classify_validate_exception;
using orca::ExceptionClassification;

namespace {
ExceptionClassification warn(int type, const std::string& msg = "")
{
    return classify_validate_exception(type, /*is_error_path=*/false, msg);
}
ExceptionClassification err(int type, const std::string& msg = "")
{
    return classify_validate_exception(type, /*is_error_path=*/true, msg);
}
} // namespace

// ============================================================================
// Filament/bed mismatch (type 1)
// ============================================================================

TEST_CASE("validate: filament-bed-mismatch warning path", "[validate]") {
    auto c = warn(orca::kExceptFilamentNotMatchBedType);
    CHECK(c.level == IssueLevel::warning);
    CHECK(c.code == "PRINT_VALIDATE_WARNING_FILAMENT_BED_MISMATCH");
    CHECK(c.fixed_message.empty());
}

TEST_CASE("validate: filament-bed-mismatch error path aborts", "[validate]") {
    auto c = err(orca::kExceptFilamentNotMatchBedType);
    CHECK(c.level == IssueLevel::error);
    CHECK(c.code == "PRINT_VALIDATE_FILAMENT_BED_MISMATCH");
    CHECK(c.continue_slicing == false);
    CHECK(c.fixed_message.empty());
}

// ============================================================================
// Temperature mismatch (type 2)
// ============================================================================

TEST_CASE("validate: temp-mismatch both paths", "[validate]") {
    CHECK(warn(orca::kExceptFilamentsDifferentTemp).code == "PRINT_VALIDATE_WARNING_FILAMENT_TEMP_MISMATCH");
    CHECK(err(orca::kExceptFilamentsDifferentTemp).code == "PRINT_VALIDATE_FILAMENT_TEMP_MISMATCH");
    CHECK(err(orca::kExceptFilamentsDifferentTemp).continue_slicing == false);
}

// ============================================================================
// Object collisions (types 3, 4)
// ============================================================================

TEST_CASE("validate: collision seq (type 3) both paths", "[validate]") {
    CHECK(warn(orca::kExceptObjectCollisionInSeqPrint).code == "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_SEQ");
    CHECK(err(orca::kExceptObjectCollisionInSeqPrint).code == "PRINT_VALIDATE_OBJECT_COLLISION_SEQ");
}

TEST_CASE("validate: collision layer (type 4) both paths", "[validate]") {
    CHECK(warn(orca::kExceptObjectCollisionInLayerPrint).code == "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_LAYER");
    CHECK(err(orca::kExceptObjectCollisionInLayerPrint).code == "PRINT_VALIDATE_OBJECT_COLLISION_LAYER");
    CHECK(err(orca::kExceptObjectCollisionInLayerPrint).continue_slicing == false);
}

// ============================================================================
// Layer height limit (type 5)
// ============================================================================

TEST_CASE("validate: layer-height-limit (type 5) both paths", "[validate]") {
    CHECK(warn(orca::kExceptLayerHeightExceedsLimit).code == "PRINT_VALIDATE_WARNING_LAYER_HEIGHT_LIMIT");
    CHECK(err(orca::kExceptLayerHeightExceedsLimit).code == "PRINT_VALIDATE_LAYER_HEIGHT_LIMIT");
}

// ============================================================================
// ORGANIC support (type 6) — escalation + fixed message asymmetry
// ============================================================================

TEST_CASE("validate: ORGANIC warning path escalates to error with fixed message", "[validate]") {
    auto c = warn(orca::kExceptOrganicSupportVariableLayer);
    CHECK(c.level == IssueLevel::error);
    CHECK(c.code == "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT");
    CHECK(!c.fixed_message.empty());            // warning path uses the canned text
    CHECK(c.fixed_message == orca::kOrganicFixedMessage);
}

TEST_CASE("validate: ORGANIC error path keeps exception message", "[validate]") {
    auto c = err(orca::kExceptOrganicSupportVariableLayer);
    CHECK(c.level == IssueLevel::error);
    CHECK(c.code == "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT");
    CHECK(c.fixed_message.empty());             // error path uses err.string
    CHECK(c.continue_slicing == false);
}

// ============================================================================
// NOT_DEFINED (type 0) — error path only, substring match, never aborts
// ============================================================================

TEST_CASE("validate: NOT_DEFINED without organic substring -> warning, continue", "[validate]") {
    auto c = err(orca::kExceptNotDefined, "some generic message");
    CHECK(c.level == IssueLevel::warning);
    CHECK(c.code == "PRINT_VALIDATE_WARNING");
    CHECK(c.continue_slicing == true);          // never aborts
}

TEST_CASE("validate: NOT_DEFINED with organic substring -> error, continue", "[validate]") {
    std::string msg = std::string("prefix ") + orca::kOrganicSubstring + " suffix";
    auto c = err(orca::kExceptNotDefined, msg);
    CHECK(c.level == IssueLevel::error);
    CHECK(c.code == "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT");
    CHECK(!c.fixed_message.empty());
    CHECK(c.continue_slicing == true);          // still never aborts even when escalated
}

TEST_CASE("validate: NOT_DEFINED empty message -> warning (no crash, no match)", "[validate]") {
    auto c = err(orca::kExceptNotDefined, "");
    CHECK(c.level == IssueLevel::warning);
    CHECK(c.code == "PRINT_VALIDATE_WARNING");
    CHECK(c.continue_slicing == true);
}

TEST_CASE("validate: NOT_DEFINED substring match is case-sensitive", "[validate]") {
    // Lowercased substring must NOT match (the check is case-sensitive find()).
    std::string lower = "variable layer height is not supported with organic supports";
    auto c = err(orca::kExceptNotDefined, lower);
    CHECK(c.level == IssueLevel::warning);      // no match -> generic warning
}

// ============================================================================
// Prime tower / mismatched variable layer height — warning path, escalated
// ============================================================================

TEST_CASE("validate: prime-tower warning path escalates to error with fixed message", "[validate]") {
    auto c = warn(orca::kExceptNotDefined, orca::kPrimeTowerSubstring);
    CHECK(c.level == IssueLevel::error);
    CHECK(c.code == "PRIME_TOWER_VARIABLE_LAYER_HEIGHT");
    CHECK(!c.fixed_message.empty());
}

TEST_CASE("validate: prime-tower error path keeps PRIME_TOWER code too", "[validate]") {
    // If libslic3r ever reports this via the error path, still escalate.
    auto c = err(orca::kExceptNotDefined, std::string("x ") + orca::kPrimeTowerSubstring + " y");
    CHECK(c.level == IssueLevel::error);
    CHECK(c.code == "PRIME_TOWER_VARIABLE_LAYER_HEIGHT");
    CHECK(c.continue_slicing == true);          // NOT_DEFINED never aborts directly
}

// ============================================================================
// Default / unknown type
// ============================================================================

TEST_CASE("validate: unknown type defaults on both paths", "[validate]") {
    CHECK(warn(999).code == "PRINT_VALIDATE_WARNING");
    CHECK(warn(999).level == IssueLevel::warning);
    CHECK(err(999).code == "PRINT_VALIDATE_ERROR");
    CHECK(err(999).level == IssueLevel::error);
    CHECK(err(999).continue_slicing == false);
}

// ============================================================================
// continue_slicing contract (error path only; warning path field is ignored)
// ============================================================================

TEST_CASE("validate: error path aborts for every non-NOT_DEFINED type", "[validate]") {
    for (int t : {orca::kExceptFilamentNotMatchBedType,
                  orca::kExceptFilamentsDifferentTemp,
                  orca::kExceptObjectCollisionInSeqPrint,
                  orca::kExceptObjectCollisionInLayerPrint,
                  orca::kExceptLayerHeightExceedsLimit,
                  orca::kExceptOrganicSupportVariableLayer})
    {
        CHECK(err(t).continue_slicing == false);
    }
    // NOT_DEFINED always continues.
    CHECK(err(orca::kExceptNotDefined).continue_slicing == true);
}
