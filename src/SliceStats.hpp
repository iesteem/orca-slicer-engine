#pragma once

#include <map>
#include <string>
#include <vector>

#include "Issue.hpp"

/**
 * @file SliceStats.hpp
 * @brief Output statistics structure for JSON export.
 */

/** Complete slice output statistics collected across all plates. */
struct SliceOutputStats
{
    /** Filament detail for each extruder. */
    struct FilamentDetail
    {
        int id;
        std::string type;         // Filament type (e.g., "PLA", "ABS")
        std::string color;        // Filament color (e.g., "#FF0000")
        double used_g;            // Used weight in grams
        double used_m;            // Used length in meters
    };

    /** Per-plate statistics. */
    struct PlateStats
    {
        int plate_id;
        bool success;
        std::string gcode_file;

        // Time statistics (in seconds)
        float total_time;
        float prepare_time;
        float print_time;         // Actual print time (excluding prepare time)

        // Filament statistics
        double total_filament_m;      // Total filament in meters
        double total_filament_g;      // Total filament in grams
        double model_filament_m;      // Model filament in meters
        double model_filament_g;      // Model filament in grams
        double total_cost;            // Total cost

        // Per-extruder filament usage
        std::map<int, double> filament_used_m;   // Per extruder: meters
        std::map<int, double> filament_used_g;   // Per extruder: grams

        // Additional info
        bool support_used;
        bool toolpath_outside;

        // Enhanced JSON output fields
        std::vector<double> nozzle_diameters;
        int plate_count;
        std::vector<FilamentDetail> filament_details;
        std::string model_thumbnail;
        std::vector<Issue> issues;
        bool long_retraction_when_cut = false;
    };

    bool success = false;
    std::string error_message;
    std::vector<PlateStats> plates;
    std::vector<Issue> issues;   // Global issues (e.g. load errors)
};
