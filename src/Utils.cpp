#include "Utils.hpp"

#include "libslic3r/libslic3r.h"

#include <boost/log/trivial.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

// Map ConfigOptionType enum to JSON-friendly string
static const char* config_type_name(int type)
{
    bool is_vector = (type & int(Slic3r::coVectorType)) != 0;
    int base = is_vector ? (type & ~int(Slic3r::coVectorType)) : type;
    switch (base) {
        case Slic3r::coFloat:  return is_vector ? "floats"           : "float";
        case Slic3r::coInt:    return is_vector ? "ints"             : "int";
        case Slic3r::coString: return is_vector ? "strings"          : "string";
        case Slic3r::coPercent:return is_vector ? "percents"         : "percent";
        case Slic3r::coFloatOrPercent: return is_vector ? "floats_or_percents" : "float_or_percent";
        case Slic3r::coBool:   return is_vector ? "bools"            : "bool";
        case Slic3r::coEnum:   return is_vector ? "enums"            : "enum";
        case Slic3r::coPoint:  return is_vector ? "points"           : "point";
        case Slic3r::coPoint3: return "point3";
        default:               return "unknown";
    }
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x",
                             static_cast<unsigned char>(c));
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string dump_config_schema(const Slic3r::ConfigDef& config_def)
{
    std::string json = "{";
    bool first_key = true;

    for (const auto& kv : config_def.options) {
        const std::string& key = kv.first;
        const Slic3r::ConfigOptionDef& def = kv.second;

        // Skip internal/disabled options with no CLI
        if (def.cli == Slic3r::ConfigOptionDef::nocli && def.label.empty())
            continue;

        if (!first_key) json += ",";
        first_key = false;
        json += "\n  \"" + key + "\": {";

        json += "\"type\":\"" + std::string(config_type_name(def.type)) + "\"";

        if (!def.label.empty())
            json += ",\"label\":\"" + json_escape(def.label) + "\"";

        // Include min if not default INT_MIN
        if (def.min > INT_MIN)
            json += ",\"min\":" + std::to_string(def.min);

        // Include max if not default INT_MAX
        if (def.max < INT_MAX)
            json += ",\"max\":" + std::to_string(def.max);

        if (def.type == Slic3r::coFloatOrPercent || def.type == Slic3r::coFloatsOrPercents)
            json += ",\"max_literal\":" + std::to_string(static_cast<int>(def.max_literal));

        if (def.nullable)
            json += ",\"nullable\":true";

        // Enum values
        if (!def.enum_values.empty()) {
            json += ",\"enum_values\":[";
            for (size_t i = 0; i < def.enum_values.size(); ++i) {
                if (i > 0) json += ",";
                json += "\"" + json_escape(def.enum_values[i]) + "\"";
            }
            json += "]";
        }

        if (!def.enum_labels.empty()) {
            json += ",\"enum_labels\":[";
            for (size_t i = 0; i < def.enum_labels.size(); ++i) {
                if (i > 0) json += ",";
                json += "\"" + json_escape(def.enum_labels[i]) + "\"";
            }
            json += "]";
        }

        // Extra U1-specific enum values
        if (!def.enum_values_u1.empty()) {
            json += ",\"enum_values_u1\":[";
            for (size_t i = 0; i < def.enum_values_u1.size(); ++i) {
                if (i > 0) json += ",";
                json += "\"" + json_escape(def.enum_values_u1[i]) + "\"";
            }
            json += "]";
        }

        // ratio_over for coFloatOrPercent
        if (!def.ratio_over.empty())
            json += ",\"ratio_over\":\"" + json_escape(def.ratio_over) + "\"";

        // sidetext (unit)
        if (!def.sidetext.empty())
            json += ",\"unit\":\"" + json_escape(def.sidetext) + "\"";

        json += "}";
    }

    json += "\n}\n";
    return json;
}

void log_plate_message(const char* stage, const char* level,
                       int plate, const std::string& msg)
{
    std::string full = std::string(stage) + " " + level + ": Plate " + std::to_string(plate) + ": " + msg;
    if (std::strcmp(level, "ERROR") == 0)
        BOOST_LOG_TRIVIAL(error) << full;
    else if (std::strcmp(level, "WARNING") == 0)
        BOOST_LOG_TRIVIAL(warning) << full;
    else
        BOOST_LOG_TRIVIAL(info) << full;

    if (std::strcmp(level, "TIP") != 0)
        std::cerr << "[" << level << "] Plate " << plate << ": " << msg << std::endl;
}

std::pair<std::string, std::string> format_exception_context(const Slic3r::StringObjectException& ex)
{
    std::string obj_name;
    if (ex.object) {
        auto mo = dynamic_cast<Slic3r::ModelObject const*>(ex.object);
        if (!mo) {
            if (auto po = dynamic_cast<Slic3r::PrintObjectBase const*>(ex.object))
                mo = po->model_object();
        }
        if (mo) obj_name = " [object: " + mo->name + "]";
    }
    std::string opt_hint = ex.opt_key.empty() ? "" : " (config: " + ex.opt_key + ")";
    return {obj_name, opt_hint};
}

void print_usage(const char* program_name)
{
    std::cout << "OrcaSlicer Cloud Slicing Engine v" << SLIC3R_VERSION << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << program_name << " input.3mf [OPTIONS]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -o, --output <file>    Output file path (without extension)" << std::endl;
    std::cout << "                         Single plate: outputs {file}.gcode or {file}.gcode.3mf" << std::endl;
    std::cout << "                         All plates: outputs {file}.gcode.3mf" << std::endl;
    std::cout << "  -p, --plate <id>       Plate number to slice (1, 2, 3...)" << std::endl;
    std::cout << "                         Omit or \"all\" for all plates (default: all)" << std::endl;
    std::cout << "  -f, --format <fmt>     Output format: gcode | gcode.3mf (default: gcode.3mf)" << std::endl;
    std::cout << "                         Note: All plates always use gcode.3mf" << std::endl;
    std::cout << "  -r, --resources <dir>  Resources directory containing printer profiles" << std::endl;
    std::cout << "                         If not set, auto-detected from binary location" << std::endl;
    std::cout << "                         (../resources/, then ./resources/, then $ORCA_RESOURCES)" << std::endl;
    std::cout << "  -j, --json [file]      Output slice statistics as JSON to specified file" << std::endl;
    std::cout << "                         If not specified, JSON is auto-saved next to the output" << std::endl;
    std::cout << "  -t, --timeout <sec>    Slicing timeout in seconds (0 = no limit)" << std::endl;
    std::cout << "  --max-size <mb>        Max input file size in MB (default: 200, 0 = no limit)" << std::endl;
    std::cout << "  --cancel-file <file>   Watchdog file for external cancellation" << std::endl;
    std::cout << "                         If the file is created, slicing is cancelled" << std::endl;
    std::cout << "  --threads <N>          Limit TBB thread count to N cores (default: all cores)" << std::endl;
    std::cout << "  --allow-custom-presets          Allow both custom printer and filament presets" << std::endl;
    std::cout << "  --allow-custom-printer-presets  Allow custom printer presets" << std::endl;
    std::cout << "  --allow-custom-filament-presets Allow custom filament presets" << std::endl;
    std::cout << "                                  (default: all presets substituted to official)" << std::endl;
    std::cout << "  --log                  Enable log file output (auto-saved next to the output)" << std::endl;
    std::cout << "  --log-file <file>      Specify log file path (implies --log)" << std::endl;
    std::cout << "  -v, --verbose          Enable verbose logging" << std::endl;
    std::cout << "  -h, --help             Show this help message" << std::endl;
    std::cout << "  --dump-config-schema   Dump all config option definitions as JSON and exit" << std::endl;
    std::cout << std::endl;
    std::cout << "Exit codes:" << std::endl;
    std::cout << "  0  Success" << std::endl;
    std::cout << "  1  Invalid arguments" << std::endl;
    std::cout << "  2  Input file not found" << std::endl;
    std::cout << "  3  3MF load / format validation error" << std::endl;
    std::cout << "  4  Slicing error (incl. timeout)" << std::endl;
    std::cout << "  5  G-code export error" << std::endl;
    std::cout << "  6  Pre-processing validation error (collision, invalid config, geometry defects)" << std::endl;
    std::cout << "  7  Post-processing warning (toolpath outside print volume)" << std::endl;
    std::cout << std::endl;
    std::cout << "Output:" << std::endl;
    std::cout << "  On success, outputs JSON with slicing statistics including:" << std::endl;
    std::cout << "    - success: true/false" << std::endl;
    std::cout << "    - plates[].time: total, prepare, print time (seconds and formatted)" << std::endl;
    std::cout << "    - plates[].filament: total/model filament (m, g), cost, per-extruder usage" << std::endl;
    std::cout << "    - plates[].gcode_file: path to output file" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program_name << " model.3mf                        # All plates -> model.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1                   # Plate 1 -> model-p1.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1 -f gcode          # Plate 1 -> model-p1.gcode (plain text)" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1 -o output         # Plate 1 -> output.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -o result              # All plates -> result.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -j stats.json          # Output statistics to stats.json" << std::endl;
}

void default_status_callback(
    const Slic3r::PrintBase::SlicingStatus& status,
    Slic3r::PrintBase* print,
    const std::string* cancel_file)
{
    // Check for external cancellation via watchdog file
    if (print && cancel_file && !cancel_file->empty()) {
        if (boost::filesystem::exists(*cancel_file)) {
            print->cancel();
            std::cout << "[Status] Cancellation requested via " << *cancel_file << std::endl;
            return;
        }
    }

    if (status.percent >= 0) {
        std::cout << "[Progress] " << status.percent << "% - " << status.text << std::endl;
    } else {
        std::cout << "[Status] " << status.text << std::endl;
    }
}

std::string format_time_hhmmss(float seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        return "00:00:00";

    constexpr int SECONDS_PER_HOUR   = 3600;
    constexpr int SECONDS_PER_MINUTE = 60;

    const int total_secs = static_cast<int>(seconds);
    const int hours = total_secs / SECONDS_PER_HOUR;
    const int mins  = (total_secs % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
    const int secs  = total_secs % SECONDS_PER_MINUTE;

    std::array<char, 32> buf;
    snprintf(buf.data(), buf.size(), "%02d:%02d:%02d", hours, mins, secs);
    return std::string(buf.data());
}

std::string generate_output_path(
    const std::string& input_file,
    const std::string& output_base,
    int plate_id,
    OutputFormat format,
    bool single_plate)
{
    boost::filesystem::path input_path(input_file);
    std::string base_name;

    if (!output_base.empty()) {
        base_name = output_base;
    } else {
        boost::filesystem::path parent = input_path.parent_path();
        if (parent.empty())
            parent = boost::filesystem::current_path();
        base_name = (parent / input_path.stem()).string();
    }

    std::string extension = (format == OutputFormat::GCODE_3MF) ? ".gcode.3mf" : ".gcode";

    std::string path;
    if (single_plate) {
        if (output_base.empty()) {
            path = base_name + "-p" + std::to_string(plate_id) + extension;
        } else {
            path = base_name + extension;
        }
    } else {
        path = base_name + ".gcode.3mf";
    }

    // Prevent multi-process collision: append unique suffix if output file already exists
    if (boost::filesystem::exists(path)) {
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        boost::filesystem::path p(path);
        path = (p.parent_path() / (p.stem().string() + "_" + std::to_string(ts))).string() + p.extension().string();
    }

    return path;
}

Slic3r::ThumbnailData resize_thumbnail(
    const Slic3r::ThumbnailData& src,
    unsigned int target_width,
    unsigned int target_height)
{
    Slic3r::ThumbnailData dst;
    dst.set(target_width, target_height);

    if (src.width == target_width && src.height == target_height) {
        std::copy(src.pixels.begin(), src.pixels.end(), dst.pixels.begin());
        return dst;
    }

    const double scale_x = static_cast<double>(src.width) / target_width;
    const double scale_y = static_cast<double>(src.height) / target_height;

    for (unsigned int dy = 0; dy < target_height; ++dy) {
        for (unsigned int dx = 0; dx < target_width; ++dx) {
            double sx = (dx + 0.5) * scale_x - 0.5;
            double sy = (dy + 0.5) * scale_y - 0.5;

            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx >= src.width - 1) sx = src.width - 1.001;
            if (sy >= src.height - 1) sy = src.height - 1.001;

            unsigned int x0 = static_cast<unsigned int>(sx);
            unsigned int y0 = static_cast<unsigned int>(sy);
            unsigned int x1 = x0 + 1;
            unsigned int y1 = y0 + 1;
            if (x1 >= src.width)  x1 = src.width - 1;
            if (y1 >= src.height) y1 = src.height - 1;

            double fx = sx - x0;
            double fy = sy - y0;

            for (int c = 0; c < 4; ++c) {
                double v00 = src.pixels[(y0 * src.width + x0) * 4 + c];
                double v10 = src.pixels[(y0 * src.width + x1) * 4 + c];
                double v01 = src.pixels[(y1 * src.width + x0) * 4 + c];
                double v11 = src.pixels[(y1 * src.width + x1) * 4 + c];

                double val = v00 * (1 - fx) * (1 - fy)
                           + v10 * fx * (1 - fy)
                           + v01 * (1 - fx) * fy
                           + v11 * fx * fy;

                dst.pixels[(dy * target_width + dx) * 4 + c] =
                    static_cast<unsigned char>(std::min(255.0, std::max(0.0, val)));
            }
        }
    }
    return dst;
}
