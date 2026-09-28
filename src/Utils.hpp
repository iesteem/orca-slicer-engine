#pragma once

#include <string>
#include <utility>

#include <boost/filesystem.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Print.hpp"

#include "Types.hpp"

// Emit a structured log line via Boost.Log.
void log_plate_message(const char* stage, const char* level, int plate, const std::string& msg);

// Extract human-readable object name and config hint from a StringObjectException
std::pair<std::string, std::string> format_exception_context(const Slic3r::StringObjectException& ex);

// Simple progress callback for cloud environment.
// When print and cancel_file are provided, the callback periodically checks
// for the existence of cancel_file and triggers print->cancel() if found.
void default_status_callback(const Slic3r::PrintBase::SlicingStatus& status, Slic3r::PrintBase* print = nullptr,
                             const std::string* cancel_file = nullptr);

// Format time in seconds as HH:MM:SS
std::string format_time_hhmmss(float seconds);

// Generate output filename based on parameters
std::string generate_output_path(const std::string& input_file, const std::string& output_base, int plate_id,
                                 OutputFormat format, bool single_plate);

// Ensure a path ends with the given extension (e.g. ".json", ".log").
// If the path already has that extension, it is returned unchanged.
// Otherwise the extension is appended.
std::string ensure_extension(const std::string& path, const std::string& ext);

// Setup a Boost.Log file sink using the canonical format shared by
// main.cpp and slic3r_c_api.cpp (cross-entry-point consistency).
// level: 0=fatal..5=trace (matches libslic3r convention).
void add_log_file_sink(const std::string& file_path, unsigned int level);

// Bilinear resize of RGBA thumbnail data to target dimensions
Slic3r::ThumbnailData resize_thumbnail(const Slic3r::ThumbnailData& src, unsigned int target_width,
                                       unsigned int target_height);

// compute_column_count() has moved to PlateGrid.hpp (kept dependency-light so
// it can be unit-tested without libslic3r). Re-exported here for callers that
// still expect it via Utils.hpp.
#include "PlateGrid.hpp"
using orca::compute_column_count;
