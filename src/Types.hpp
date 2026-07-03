#pragma once

#include <map>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

// Exit codes
constexpr int EXIT_OK = 0;
constexpr int EXIT_INVALID_ARGS = 1;
constexpr int EXIT_FILE_NOT_FOUND = 2;
constexpr int EXIT_LOAD_ERROR = 3;
constexpr int EXIT_SLICING_ERROR = 4;
constexpr int EXIT_EXPORT_ERROR = 5;
constexpr int EXIT_PREPROCESS_ERROR = 6; // pre-slicing validation failure (config/input/presets invalid)
constexpr int EXIT_POSTPROCESS_ERROR = 7; // post-slicing fatal error (GCode generated but unusable)

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
        int id;
        std::string type; // Filament type (e.g., "PLA", "ABS")
        std::string color; // Filament color (e.g., "#FF0000")
        double used_g; // Used weight in grams
        double used_m; // Used length in meters
    };

    // Per-plate statistics
    struct PlateStats
    {
        int plate_id;
        bool success;
        std::string gcode_file;

        // Time statistics (in seconds)
        float total_time;
        float prepare_time;
        float print_time; // Actual print time (excluding prepare time)

        // Filament statistics
        double total_filament_m; // Total filament in meters
        double total_filament_g; // Total filament in grams
        double model_filament_m; // Model filament in meters
        double model_filament_g; // Model filament in grams
        double total_cost; // Total cost

        // Per-extruder filament usage
        std::map<int, double> filament_used_m; // Per extruder: meters
        std::map<int, double> filament_used_g; // Per extruder: grams

        // Additional info
        bool support_used;
        bool toolpath_outside;

        // New fields for enhanced JSON output
        std::vector<double> nozzle_diameters; // Nozzle diameter for each extruder
        int plate_count; // Total number of plates
        std::vector<FilamentDetail> filament_details; // Detailed filament info per extruder
        std::string model_thumbnail; // Model thumbnail path within gcode.3mf (Metadata/plate_N.png)
        std::vector<Issue> issues; // Per-plate issues (errors/warnings/tips)
        bool long_retraction_when_cut = false; // Experimental long retraction when cut feature active
    };

    bool success = false;
    std::string error_message;
    std::vector<PlateStats> plates;
    std::vector<Issue> issues; // Global issues (e.g. load errors)
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
