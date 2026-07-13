#pragma once

#include <cmath>
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

// Bilinear resize of RGBA thumbnail data to target dimensions
Slic3r::ThumbnailData resize_thumbnail(const Slic3r::ThumbnailData& src, unsigned int target_width,
                                       unsigned int target_height);

// Compute column count for plate grid layout (matches GUI's PartPlate.hpp logic)
inline int compute_column_count(int count)
{
    float value = sqrt(static_cast<float>(count));
    float round_value = round(value);
    return (value > round_value) ? (round_value + 1) : round_value;
}
