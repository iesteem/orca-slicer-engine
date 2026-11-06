#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "slic3r_c_api.h" // SLIC3R_* status codes — single source for EXIT_*

// Exit codes — C++ aliases of the SLIC3R_* codes defined in slic3r_c_api.h
// (the C ABI header is the single source of truth; slic3r_slice() returns
// these values untranslated, so the two families can never drift).
constexpr int EXIT_OK = SLIC3R_OK;
constexpr int EXIT_INVALID_ARGS = SLIC3R_ERR_ARGS;
constexpr int EXIT_FILE_NOT_FOUND = SLIC3R_ERR_FILE_NOT_FOUND;
constexpr int EXIT_LOAD_ERROR = SLIC3R_ERR_LOAD;
constexpr int EXIT_SLICING_ERROR = SLIC3R_ERR_SLICING;
constexpr int EXIT_EXPORT_ERROR = SLIC3R_ERR_EXPORT;
constexpr int EXIT_PREPROCESS_ERROR = SLIC3R_ERR_PREPROCESS; // pre-slicing validation failure (config/input/presets invalid)
constexpr int EXIT_POSTPROCESS_ERROR = SLIC3R_ERR_POSTPROCESS; // post-slicing fatal error (GCode generated but unusable)
constexpr int EXIT_INTERNAL = SLIC3R_ERR_INTERNAL; // C++ exception crossing the API boundary

// Sentinel plate_id for issues not associated with a specific plate (global errors/warnings)
constexpr int GLOBAL_ISSUE = -1;

// Preset default values — used as fallbacks when config keys are absent
constexpr double DEFAULT_NOZZLE_DIAMETER = 0.4;
constexpr const char* DEFAULT_PRINTER_MODEL = "Snapmaker U1";

// Output format enum
enum class OutputFormat
{
    GCODE,
    GCODE_3MF
};

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

// Output statistics structure for JSON export
struct SliceOutputStats
{
    // Filament detail for each extruder
    struct FilamentDetail
    {
        int id = -1; // Extruder slot, 1-based; -1 = unknown/unset
        std::string type; // Filament type (e.g., "PLA", "ABS")
        std::string color; // Filament color (e.g., "#FF0000")
        double used_g = 0.0; // Used weight in grams
        double used_m = 0.0; // Used length in meters
    };

    // Per-plate statistics
    struct PlateStats
    {
        // All scalar members carry in-class defaults: every construction site
        // default-constructs the struct and failed plates return early from
        // assemble_plate_stats before most fields are assigned — without
        // defaults those fields hold indeterminate values that the JSON
        // aggregation would read.
        int plate_id = -1; // 1-based plate index; -1 = unknown/unset
        bool success = false;
        std::string gcode_file;

        // Time statistics (in seconds)
        float total_time = 0.0f;
        float prepare_time = 0.0f;
        float print_time = 0.0f; // Actual print time (excluding prepare time)

        // Filament statistics
        double total_filament_m = 0.0; // Total filament in meters
        double total_filament_g = 0.0; // Total filament in grams
        double model_filament_m = 0.0; // Model filament in meters
        double model_filament_g = 0.0; // Model filament in grams
        double total_cost = 0.0; // Total cost

        // Per-extruder filament usage
        std::map<int, double> filament_used_m; // Per extruder: meters
        std::map<int, double> filament_used_g; // Per extruder: grams

        // Additional info
        // G-code actual layer: support extrusion volume > 0 (source
        // GCodeProcessor support_volumes_per_extruder), not the config flag.
        bool support_generated = false;
        bool toolpath_outside = false;

        // New fields for enhanced JSON output
        std::vector<double> nozzle_diameters; // Nozzle diameter for each extruder
        int plate_count = 0; // Total number of plates
        std::vector<FilamentDetail> filament_details; // Detailed filament info per extruder
        std::string model_thumbnail; // Model thumbnail path within gcode.3mf (Metadata/plate_N.png)
        std::vector<Issue> issues; // Per-plate issues (errors/warnings/tips)
        bool long_retraction_when_cut = false; // Experimental long retraction when cut feature active
    };

    bool success = false;
    std::string error_message;
    // Verbatim mirror of the process/C-API return code (SliceEngine::
    // exit_code()), stamped at the tail of build_statistics(). Emitted as-is:
    // exit_code() is a monotonic ratchet over set_error_type stamps, so the
    // value may EXCEED the code implied by the visible issues — never
    // recompute it from the issue list. Invariant success == (exit_code == 0)
    // holds on run()-driven (JSON-emitting) paths only; the test-only prefix
    // entry points (run_geometry_preprocess_only / run_preset_substitution_
    // only) can legitimately carry success=false with exit_code=0 — do not
    // "fix" that, they never emit JSON.
    int exit_code = 0;
    std::vector<PlateStats> plates;
    std::vector<Issue> issues; // Global issues (e.g. load errors)
    // Input-file property: any object/part/painted facet/height-range in the
    // 3MF references a virtual mixed-filament slot (extruder id above the
    // physical filament_colour count). Determined at load time; orthogonal to
    // slicing success.
    bool mixed_filament_used = false;
    // Verbatim "version" string from the vendor root JSON
    // (<resources>/profiles/Snapmaker.json), read at report time — no Semver
    // normalisation, so "02.03.03.03" stays zero-padded. Orthogonal to slicing
    // success: populated on failure exits too. Empty when the file could not
    // be read; the JSON report then omits resources_version.
    std::string resources_version;
};

// RAII guard that removes a list of temp files on destruction
struct TempFileGuard
{
    std::vector<std::string>& mFiles;
    explicit TempFileGuard(std::vector<std::string>& f) : mFiles(f)
    {
    }
    ~TempFileGuard()
    {
        cleanup();
    }
    void cleanup()
    {
        // Forensics escape hatch (precedent P-27): when a run fails, the
        // per-plate plate_N.gcode files are the primary evidence for diffing
        // conflict/outside footprints against a desktop reference, but this
        // guard deletes them on unwind. ORCA_KEEP_FAILED_GCODE=1 keeps every
        // tracked temp file and prints its path to stderr. Debug aid only —
        // callers must not depend on it: kept files sit in the per-run temp
        // dir, which the OS/next cleanup pass may still sweep.
        if (std::getenv("ORCA_KEEP_FAILED_GCODE") != nullptr)
        {
            for (const auto& file : mFiles)
                std::fprintf(stderr, "[ORCA_KEEP_FAILED_GCODE] kept: %s\n", file.c_str());
            return;
        }
        for (const auto& file : mFiles)
        {
            try
            {
                if (boost::filesystem::exists(file))
                    boost::filesystem::remove(file);
            }
            catch (...)
            {
            }
        }
    }
    // Prevent copy
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};
