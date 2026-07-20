#include "Utils.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/support/date_time.hpp>

#include "libslic3r/libslic3r.h"

void log_plate_message(const char* stage, const char* level, int plate, const std::string& msg)
{
    std::string full = std::string(stage) + " " + level + ": Plate " + std::to_string(plate) + ": " + msg;
    if (std::strcmp(level, "ERROR") == 0)
        BOOST_LOG_TRIVIAL(error) << full;
    else if (std::strcmp(level, "WARNING") == 0)
        BOOST_LOG_TRIVIAL(warning) << full;
    else
        BOOST_LOG_TRIVIAL(info) << full;
}

std::pair<std::string, std::string> format_exception_context(const Slic3r::StringObjectException& ex)
{
    std::string obj_name;
    if (ex.object)
    {
        auto mo = dynamic_cast<Slic3r::ModelObject const*>(ex.object);
        if (!mo)
        {
            if (auto po = dynamic_cast<Slic3r::PrintObjectBase const*>(ex.object))
                mo = po->model_object();
        }
        if (mo)
            obj_name = " [object: " + mo->name + "]";
    }
    std::string opt_hint = ex.opt_key.empty() ? "" : " (config: " + ex.opt_key + ")";
    return {obj_name, opt_hint};
}

void default_status_callback(const Slic3r::PrintBase::SlicingStatus& status, Slic3r::PrintBase* print,
                             const std::string* cancel_file)
{
    // Check for external cancellation via watchdog file
    if (print && cancel_file && !cancel_file->empty())
    {
        if (boost::filesystem::exists(*cancel_file))
        {
            print->cancel();
            std::cout << "[Status] Cancellation requested via " << *cancel_file << std::endl;
            return;
        }
    }

    if (status.percent >= 0)
    {
        std::cout << "[Progress] " << status.percent << "% - " << status.text << std::endl;
    }
    else
    {
        std::cout << "[Status] " << status.text << std::endl;
    }
}

std::string format_time_hhmmss(float seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        return "00:00:00";
    int total_secs = static_cast<int>(seconds);
    int hours = total_secs / 3600;
    int mins = (total_secs % 3600) / 60;
    int secs = total_secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, mins, secs);
    return std::string(buf);
}

std::string ensure_extension(const std::string& path, const std::string& ext)
{
    if (path.empty())
    {
        return path;
    }
    boost::filesystem::path p(path);
    if (p.extension() == ext)
    {
        return path;
    }
    return path + ext;
}

std::string generate_output_path(const std::string& input_file, const std::string& output_base, int plate_id,
                                 OutputFormat format, bool single_plate)
{
    boost::filesystem::path input_path(input_file);
    std::string base_name;

    if (!output_base.empty())
    {
        base_name = output_base;
    }
    else
    {
        boost::filesystem::path parent = input_path.parent_path();
        if (parent.empty())
            parent = boost::filesystem::current_path();
        base_name = (parent / input_path.stem()).string();
    }

    std::string extension = (format == OutputFormat::GCODE_3MF) ? ".gcode.3mf" : ".gcode";

    std::string path;
    if (single_plate)
    {
        if (output_base.empty())
        {
            path = base_name + "-p" + std::to_string(plate_id) + extension;
        }
        else
        {
            path = base_name + extension;
        }
    }
    else
    {
        path = base_name + ".gcode.3mf";
    }

    // Prevent multi-process collision: append unique suffix if output file already exists
    if (boost::filesystem::exists(path))
    {
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        boost::filesystem::path p(path);
        path = (p.parent_path() / (p.stem().string() + "_" + std::to_string(ts))).string() + p.extension().string();
    }

    return path;
}

Slic3r::ThumbnailData resize_thumbnail(const Slic3r::ThumbnailData& src, unsigned int target_width,
                                       unsigned int target_height)
{
    Slic3r::ThumbnailData dst;
    dst.set(target_width, target_height);

    // Guard against zero-dimension inputs: empty thumbnails produce no output
    if (src.width == 0 || src.height == 0 || target_width == 0 || target_height == 0)
        return dst;

    if (src.width == target_width && src.height == target_height)
    {
        std::copy(src.pixels.begin(), src.pixels.end(), dst.pixels.begin());
        return dst;
    }

    const double scale_x = static_cast<double>(src.width) / target_width;
    const double scale_y = static_cast<double>(src.height) / target_height;

    for (unsigned int dy = 0; dy < target_height; ++dy)
    {
        for (unsigned int dx = 0; dx < target_width; ++dx)
        {
            double sx = (dx + 0.5) * scale_x - 0.5;
            double sy = (dy + 0.5) * scale_y - 0.5;

            if (sx < 0)
                sx = 0;
            if (sy < 0)
                sy = 0;
            if (sx >= src.width - 1)
                sx = src.width - 1.001;
            if (sy >= src.height - 1)
                sy = src.height - 1.001;

            unsigned int x0 = static_cast<unsigned int>(sx);
            unsigned int y0 = static_cast<unsigned int>(sy);
            unsigned int x1 = x0 + 1;
            unsigned int y1 = y0 + 1;
            if (x1 >= src.width)
                x1 = src.width - 1;
            if (y1 >= src.height)
                y1 = src.height - 1;

            double fx = sx - x0;
            double fy = sy - y0;

            for (int c = 0; c < 4; ++c)
            {
                double v00 = src.pixels[(y0 * src.width + x0) * 4 + c];
                double v10 = src.pixels[(y0 * src.width + x1) * 4 + c];
                double v01 = src.pixels[(y1 * src.width + x0) * 4 + c];
                double v11 = src.pixels[(y1 * src.width + x1) * 4 + c];

                double val =
                    v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) + v01 * (1 - fx) * fy + v11 * fx * fy;

                dst.pixels[(dy * target_width + dx) * 4 + c] =
                    static_cast<unsigned char>(std::min(255.0, std::max(0.0, val)));
            }
        }
    }
    return dst;
}

void add_log_file_sink(const std::string& file_path, unsigned int level)
{
    namespace logging = boost::log;
    namespace keywords = boost::log::keywords;
    namespace expr = boost::log::expressions;
    namespace attrs = boost::log::attributes;

    logging::add_file_log(
        keywords::file_name = file_path,
        keywords::format =
            (expr::stream << "[" << expr::attr<logging::trivial::severity_level>("Severity") << "]\t"
                          << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                          << "[Thread " << expr::attr<attrs::current_thread_id::value_type>("ThreadID") << "]"
                          << ":" << expr::smessage));
    logging::add_common_attributes();

    // Map libslic3r integer level (0=fatal..5=trace) to Boost severity enum.
    // Boost.Log enum values: trace=0, debug=1, info=2, warning=3, error=4, fatal=5.
    // libslic3r mapping: 0=fatal, 1=error, 2=warning, 3=info, 4=debug, 5=trace.
    boost::log::trivial::severity_level sev;
    switch (level)
    {
    case 0: sev = boost::log::trivial::fatal;   break;
    case 1: sev = boost::log::trivial::error;   break;
    case 2: sev = boost::log::trivial::warning; break;
    case 3: sev = boost::log::trivial::info;    break;
    case 4: sev = boost::log::trivial::debug;   break;
    case 5: sev = boost::log::trivial::trace;   break;
    default: sev = boost::log::trivial::info;   break;
    }
    logging::core::get()->set_filter(logging::trivial::severity >= sev);
}
