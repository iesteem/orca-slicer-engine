#pragma once

#include <string>

// Sentinel plate_id for issues not associated with a specific plate (global errors/warnings)
// inline constexpr (R2, 2026-11-10): namespace-scope constexpr defaults to internal
// linkage (per-TU copies; see 头文件规则 §1 问2). Not odr-used today, but the
// rest of the repo (PlateGrid/ValidateClassify) already uses inline constexpr.
inline constexpr int GLOBAL_ISSUE = -1;

// Issue severity levels — ordinal defines sort / display order (high to low)
enum class IssueLevel
{
    error = 0,
    serious_warning = 1,
    warning = 2,
    tip = 3
};

// Structured issue for error/warning/tip collection
struct Issue
{
    // NSDMI on every scalar (R1, 2026-11-10): production code constructs Issue
    // exclusively through the make_* factories below, which fully initialise
    // via aggregate init — but a bare `Issue iss;` left level/plate_id/
    // z_height as indeterminate values. C++17 keeps this an aggregate, so
    // the factory call sites are unaffected.
    IssueLevel level = IssueLevel::warning;
    int plate_id = GLOBAL_ISSUE; // plate index, -1 for global
    std::string object_name; // related object, empty if N/A
    double z_height = -1.0; // Z-level in mm, -1 if N/A
    std::string code; // error code (e.g. from SliceWarning), empty if N/A
    std::string message; // human-readable description
    std::string suggestion; // actionable repair suggestion, empty if N/A
    int layer = -1; // 0-based layer index, -1 if N/A
};

// Factory functions — set sensible defaults (z_height=-1, correct level)
inline Issue make_error(int plate_id, const std::string& code, const std::string& message,
                        const std::string& object_name = "", const std::string& suggestion = "")
{
    return Issue{IssueLevel::error, plate_id, object_name, -1.0, code, message, suggestion};
}
inline Issue make_warning(int plate_id, const std::string& code, const std::string& message,
                          const std::string& object_name = "", const std::string& suggestion = "")
{
    return Issue{IssueLevel::warning, plate_id, object_name, -1.0, code, message, suggestion};
}
inline Issue make_tip(int plate_id, const std::string& code, const std::string& message)
{
    return Issue{IssueLevel::tip, plate_id, "", -1.0, code, message, ""};
}
inline Issue make_serious_warning(int plate_id, const std::string& code, const std::string& message,
                                  const std::string& object_name = "", const std::string& suggestion = "")
{
    return Issue{IssueLevel::serious_warning, plate_id, object_name, -1.0, code, message, suggestion};
}
