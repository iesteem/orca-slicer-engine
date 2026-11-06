#pragma once

#include <string>

// Sentinel plate_id for issues not associated with a specific plate (global errors/warnings)
constexpr int GLOBAL_ISSUE = -1;

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
    IssueLevel level;
    int plate_id; // plate index, -1 for global
    std::string object_name; // related object, empty if N/A
    double z_height; // Z-level in mm, -1 if N/A
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
