#pragma once

// Pure classification of Print::validate exceptions into issue metadata,
// extracted from SliceEngine::emit_validate_warning / emit_validate_error for
// unit testing. Only depends on Types.hpp (IssueLevel) + <string> — no
// libslic3r — so it compiles into the lightweight engine-tests target.
//
// libslic3r's StringExceptionType is a plain enum with explicit integer
// constants 0..6 (PrintBase.hpp:19-28), so the caller passes
// static_cast<int>(exception.type) and this header stays libslic3r-free.

#include <string>

#include "Types.hpp"

namespace orca {

// StringExceptionType integer values (PrintBase.hpp:19-27). Reproduced as
// named constants so this header does not include libslic3r.
inline constexpr int kExceptNotDefined                    = 0;
inline constexpr int kExceptFilamentNotMatchBedType       = 1;
inline constexpr int kExceptFilamentsDifferentTemp        = 2;
inline constexpr int kExceptObjectCollisionInSeqPrint     = 3;
inline constexpr int kExceptObjectCollisionInLayerPrint   = 4;
inline constexpr int kExceptLayerHeightExceedsLimit       = 5;
inline constexpr int kExceptOrganicSupportVariableLayer   = 6;

// Result of classifying one validate exception. The SliceEngine thin wrapper:
//   - picks make_error vs make_warning by `level`,
//   - builds the Issue message from `fixed_message` when non-empty, otherwise
//     from the exception string (+ opt_hint from format_exception_context),
//   - on the error path, returns cls.continue_slicing (true = keep going).
//     continue_slicing is meaningless on the warning path (emit_validate_warning
//     returns void) and should be ignored there.
struct ExceptionClassification
{
    IssueLevel  level            = IssueLevel::warning;
    std::string code;
    bool        continue_slicing = true;
    std::string fixed_message;   // non-empty => use this verbatim as the message
};

// Fixed organic-support message/suggestion, used when an ORGANIC classification
// escalates to error with a canned text (the warning-path ORGANIC branch and
// the NOT_DEFINED+substring branch in emit_validate_error). Exposed so the thin
// wrapper can fill the matching suggestion field.
inline constexpr const char* kOrganicFixedMessage =
    "Organic supports do not support variable layer height. "
    "Please disable variable layer height or switch to non-organic support type.";
inline constexpr const char* kOrganicFixedSuggestion =
    "In Snapmaker Orca, disable variable layer height or change support type to default (non-organic).";

// Substring that distinguishes the organic-support case inside a NOT_DEFINED
// (type 0) exception message (SliceEngine.cpp emit_validate_error).
inline constexpr const char* kOrganicSubstring =
    "Variable layer height is not supported with Organic supports";

// Substring that distinguishes the build-volume-height case inside a NOT_DEFINED
// (type 0) exception message. Print.cpp:1642-1657 returns these as hard errors
// (desktop blocks slicing with an error dialog), but the exception type is
// STRING_EXCEPT_NOT_DEFINED — without this match the default NOT_DEFINED rule
// downgrades them to a warning and slicing continues, deferring the failure to
// post-processing toolpath checks.
inline constexpr const char* kBuildVolumeHeightSubstring =
    "exceeds the maximum build volume height";

// Classify a validate exception.
//   exception_type : static_cast<int>(StringObjectException::type), 0..6
//   is_error_path  : true for emit_validate_error (PRINT_VALIDATE_* codes),
//                    false for emit_validate_warning (PRINT_VALIDATE_WARNING_* codes)
//   message        : the exception's string; only consulted for type 0 (NOT_DEFINED)
//                    substring matching on the error path.
ExceptionClassification classify_validate_exception(
    int                exception_type,
    bool               is_error_path,
    const std::string& message)
{
    ExceptionClassification c;
    c.continue_slicing = !is_error_path ? true : false; // error path defaults to abort

    switch (exception_type)
    {
    case kExceptFilamentNotMatchBedType:
        c.level = is_error_path ? IssueLevel::error : IssueLevel::warning;
        c.code  = is_error_path ? "PRINT_VALIDATE_FILAMENT_BED_MISMATCH"
                                : "PRINT_VALIDATE_WARNING_FILAMENT_BED_MISMATCH";
        break;
    case kExceptFilamentsDifferentTemp:
        c.level = is_error_path ? IssueLevel::error : IssueLevel::warning;
        c.code  = is_error_path ? "PRINT_VALIDATE_FILAMENT_TEMP_MISMATCH"
                                : "PRINT_VALIDATE_WARNING_FILAMENT_TEMP_MISMATCH";
        break;
    case kExceptObjectCollisionInSeqPrint:
        c.level = is_error_path ? IssueLevel::error : IssueLevel::warning;
        c.code  = is_error_path ? "PRINT_VALIDATE_OBJECT_COLLISION_SEQ"
                                : "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_SEQ";
        break;
    case kExceptObjectCollisionInLayerPrint:
        c.level = is_error_path ? IssueLevel::error : IssueLevel::warning;
        c.code  = is_error_path ? "PRINT_VALIDATE_OBJECT_COLLISION_LAYER"
                                : "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_LAYER";
        break;
    case kExceptLayerHeightExceedsLimit:
        c.level = is_error_path ? IssueLevel::error : IssueLevel::warning;
        c.code  = is_error_path ? "PRINT_VALIDATE_LAYER_HEIGHT_LIMIT"
                                : "PRINT_VALIDATE_WARNING_LAYER_HEIGHT_LIMIT";
        break;
    case kExceptOrganicSupportVariableLayer:
        // Warning path escalates this type to error with a fixed message; the
        // error path keeps the exception's own message (fixed_message empty).
        c.level         = IssueLevel::error;
        c.code          = "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT";
        c.fixed_message = is_error_path ? std::string{} : std::string(kOrganicFixedMessage);
        break;
    case kExceptNotDefined:
        if (is_error_path)
        {
            // NOT_DEFINED never aborts slicing (falls through to later checks),
            // except for the substring-classified hard errors below.
            c.continue_slicing = true;
            if (message.find(kOrganicSubstring) != std::string::npos)
            {
                c.level         = IssueLevel::error;
                c.code          = "ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT";
                c.fixed_message = kOrganicFixedMessage;
            }
            else if (message.find(kBuildVolumeHeightSubstring) != std::string::npos)
            {
                // Keep the exception's own message (fixed_message empty) — it
                // already names the object and the reason.
                c.level           = IssueLevel::error;
                c.code            = "BUILD_VOLUME_TOO_HIGH";
                c.continue_slicing = false;
            }
            else
            {
                c.level = IssueLevel::warning;
                c.code  = "PRINT_VALIDATE_WARNING";
            }
        }
        else
        {
            // NOT_DEFINED does not occur on the warning path; treat as default.
            c.level = IssueLevel::warning;
            c.code  = "PRINT_VALIDATE_WARNING";
        }
        break;
    default:
        c.level = is_error_path ? IssueLevel::error : IssueLevel::warning;
        c.code  = is_error_path ? "PRINT_VALIDATE_ERROR" : "PRINT_VALIDATE_WARNING";
        break;
    }

    return c;
}

} // namespace orca
