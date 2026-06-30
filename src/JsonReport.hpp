#pragma once

#include <string>

struct SliceOutputStats;

// Build the slice-statistics JSON document and return it as a string.
// Single source of truth for the statistics schema (base-1 plate/filament ids,
// print_info_total aggregation). Both the stdout/file path below and the C API
// boundary serialize through this function.
std::string build_statistics_json(const SliceOutputStats& stats,
                                   const std::string& output_file_path);

// Output slice statistics as JSON to stdout and optionally to a file.
void output_slice_statistics(const SliceOutputStats& stats,
                             const std::string& json_output_path,
                             const std::string& output_file_path);
