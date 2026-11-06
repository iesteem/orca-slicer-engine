#pragma once

#include <map>
#include <string>
#include <vector>

#include "Issue.hpp"

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
        float print_time = 0.0f; // Actual print time (excluding prepare time)

        // Filament statistics
        double total_filament_m = 0.0; // Total filament in meters
        double total_filament_g = 0.0; // Total filament in grams

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
