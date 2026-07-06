#pragma once

#include <string>

/**
 * @file Issue.hpp
 * @brief Structured issue type and factory functions for error/warning/tip collection.
 */

/** Output format for sliced result. */
enum class OutputFormat
{
    GCODE,
    GCODE_3MF
};

/** Structured issue for error/warning/tip collection. */
struct Issue
{
    std::string level;       // "error" | "warning" | "tip"
    int plate_id;            // plate index, -1 for global
    std::string object_name; // related object, empty if N/A
    double z_height;         // Z-level in mm, -1 if N/A
    std::string code;        // error code (e.g. from SliceWarning), empty if N/A
    std::string message;     // human-readable description
    std::string suggestion;  // actionable repair suggestion, empty if N/A
};

/**
 * Create an error-level Issue.
 *
 * @param plate_id    Plate index (-1 for global).
 * @param code        Error code string.
 * @param message     Human-readable description.
 * @param object_name Related object name (optional).
 * @param suggestion  Repair suggestion (optional).
 * @return Issue with level="error" and z_height=-1.0.
 */
inline Issue make_error(int plate_id, const std::string& code,
                        const std::string& message,
                        const std::string& object_name = "",
                        const std::string& suggestion = "")
{
    return Issue{"error", plate_id, object_name, -1.0, code, message, suggestion};
}

/**
 * Create a warning-level Issue.
 *
 * @param plate_id    Plate index (-1 for global).
 * @param code        Warning code string.
 * @param message     Human-readable description.
 * @param object_name Related object name (optional).
 * @param suggestion  Repair suggestion (optional).
 * @return Issue with level="warning" and z_height=-1.0.
 */
inline Issue make_warning(int plate_id, const std::string& code,
                          const std::string& message,
                          const std::string& object_name = "",
                          const std::string& suggestion = "")
{
    return Issue{"warning", plate_id, object_name, -1.0, code, message, suggestion};
}

/**
 * Create a tip-level Issue (no object name or suggestion).
 *
 * @param plate_id  Plate index (-1 for global).
 * @param code      Tip code string.
 * @param message   Human-readable description.
 * @return Issue with level="tip", empty object_name and suggestion.
 */
inline Issue make_tip(int plate_id, const std::string& code,
                      const std::string& message)
{
    return Issue{"tip", plate_id, "", -1.0, code, message, ""};
}
