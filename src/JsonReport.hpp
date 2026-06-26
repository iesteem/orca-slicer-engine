#pragma once

#include <string>

struct SliceOutputStats;

/**
 * Output slice statistics as JSON.
 *
 * Writes the complete slice statistics (per-plate timing, filament usage,
 * issues) to the specified JSON file. Also outputs a summary to stdout.
 *
 * @param stats            Complete slice output statistics from the engine.
 * @param json_output_path Path to write the JSON statistics file.
 * @param output_file_path Path to the main output file (for reference in JSON).
 */
void output_slice_statistics(const SliceOutputStats& stats,
                             const std::string& json_output_path,
                             const std::string& output_file_path);
