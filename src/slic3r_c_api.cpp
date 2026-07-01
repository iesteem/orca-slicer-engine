/**
 * slic3r_c_api.cpp — Production C API boundary for libslic3r shared library.
 *
 * Internally uses the full SliceEngine pipeline. Externally exposes only C.
 * All C++ exceptions are caught at the boundary and converted to error codes.
 */

#include "slic3r_c_api.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Internal C++ includes (hidden from API consumers)
#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "nlohmann/json.hpp"
#include <boost/filesystem.hpp>

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/support/date_time.hpp>

#include "Types.hpp"
#include "SliceEngine.hpp"
#include "JsonReport.hpp"

using json = nlohmann::json;

// ====================================================================
// Internal context
// ====================================================================

struct slic3r_ctx_s {
    std::string   resources_dir;
    std::string   last_error;
    bool          initialized = false;

    // Cancellation flag (checked by run loop, set by slic3r_cancel)
    std::atomic<bool> cancel_requested{false};
};

// ====================================================================
// Helpers
// ====================================================================

static void set_error(slic3r_ctx_t* ctx, const std::string& msg) {
    if (ctx) ctx->last_error = msg;
    fprintf(stderr, "[slic3r] ERROR: %s\n", msg.c_str());
}

// Add a Boost.Log file sink writing to the exact path (no g_data_dir prefix).
// Uses the same format as libslic3r's set_log_path_and_level for consistency.
static void add_log_file_sink(const std::string& file_path, unsigned int level) {
    namespace logging = boost::log;
    namespace keywords = boost::log::keywords;
    namespace expr = boost::log::expressions;
    namespace attrs = boost::log::attributes;

    auto sink = logging::add_file_log(
        keywords::file_name = file_path,
        keywords::format =
        (
            expr::stream
            << "[" << expr::attr<logging::trivial::severity_level>("Severity") << "]\t"
            << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
            << "[Thread " << expr::attr<attrs::current_thread_id::value_type>("ThreadID") << "]"
            << ":" << expr::smessage
        )
    );
    logging::add_common_attributes();

    // Map libslic3r integer level (0=fatal..5=trace) to Boost severity enum.
    // Boost.Log enum values: trace=0, debug=1, info=2, warning=3, error=4, fatal=5.
    // libslic3r mapping: 0=fatal, 1=error, 2=warning, 3=info, 4=debug, 5=trace.
    boost::log::trivial::severity_level sev;
    switch (level) {
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

// Parse CLI params JSON into EngineConfig
static bool parse_params(const char* json_str, EngineConfig& cfg) {
    if (!json_str || !json_str[0]) return true; // empty = defaults

    try {
        json j = json::parse(json_str);
        if (j.contains("plate_id"))       cfg.plate_id       = j["plate_id"].get<int>();
        if (j.contains("timeout_seconds")) cfg.timeout_seconds = j["timeout_seconds"].get<int>();
        if (j.contains("max_size_mb"))     cfg.max_size_mb   = j["max_size_mb"].get<int>();
        if (j.contains("enforce_official_presets")) cfg.enforce_official_presets = j["enforce_official_presets"].get<bool>();
        if (j.contains("substitute_filaments"))    cfg.substitute_filaments    = j["substitute_filaments"].get<bool>();
        if (j.contains("clear_custom_gcode"))      cfg.clear_custom_gcode      = j["clear_custom_gcode"].get<bool>();
        if (j.contains("format")) {
            std::string fmt = j["format"].get<std::string>();
            cfg.format = (fmt == "gcode") ? OutputFormat::GCODE : OutputFormat::GCODE_3MF;
        }
        if (j.contains("cancel_file"))    cfg.cancel_file    = j["cancel_file"].get<std::string>();
        if (j.contains("data_dir"))       cfg.data_dir       = j["data_dir"].get<std::string>();
        if (j.contains("log_path")) {
            cfg.log_path = j["log_path"].get<std::string>();
            if (!cfg.log_path.empty()) cfg.log_enabled = true;
        }
        if (j.contains("json_output_path"))
            cfg.json_output_path = j["json_output_path"].get<std::string>();
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[slic3r] params parse error: %s\n", e.what());
        return false;
    }
}

// ====================================================================
// API Implementation
// ====================================================================

SLIC3R_API slic3r_ctx_t* slic3r_create(const char* resources_dir) {
    auto* ctx = new (std::nothrow) slic3r_ctx_t();
    if (!ctx) return nullptr;

    ctx->resources_dir = resources_dir ? resources_dir : "";

    // Initialize Slic3r global state
    Slic3r::set_resources_dir(ctx->resources_dir);
    Slic3r::set_data_dir(ctx->resources_dir + "/profiles");

    ctx->initialized = true;
    return ctx;
}

SLIC3R_API void slic3r_destroy(slic3r_ctx_t* ctx) {
    delete ctx;
}

SLIC3R_API int slic3r_slice(
    slic3r_ctx_t* ctx,
    const char*   input_3mf,
    const char*   output_base,
    const char*   params_json,
    char*         stats_out,
    size_t        stats_size)
{
    if (!ctx || !input_3mf || !output_base) {
        set_error(ctx, "NULL argument");
        return SLIC3R_ERR_ARGS;
    }

    // Reset cancellation
    ctx->cancel_requested = false;

    try {
        // Build EngineConfig
        EngineConfig cfg;
        cfg.input_file = input_3mf;
        cfg.output_base = output_base;
        cfg.cancel_file = "";  // could be passed via params_json

        // Auto-generate temp directory (same as original main.cpp)
        auto unique_dir = boost::filesystem::temp_directory_path()
            / boost::filesystem::unique_path("orca_slice_%%%%-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(unique_dir);
        cfg.temp_dir = unique_dir.string();

        if (!parse_params(params_json, cfg)) {
            set_error(ctx, "Failed to parse params JSON");
            return SLIC3R_ERR_ARGS;
        }

        if (cfg.data_dir.empty())
            cfg.data_dir = ctx->resources_dir + "/profiles";

        // Wire up Boost.Log file sink before any engine logging
        if (cfg.log_enabled && !cfg.log_path.empty()) {
            add_log_file_sink(cfg.log_path, 3);  // level 3 = info
        }

        // Run the full pipeline
        std::vector<std::string> temp_files;
        TempFileGuard temp_guard(temp_files);

        SliceEngine engine(cfg, temp_files);
        bool ran = engine.run();

        // Serialize stats (unified JsonReport schema: base-1 plate/filament ids)
        const auto& stats = engine.stats();
        std::string json_str = build_statistics_json(stats, engine.output_path());

        // Write JSON statistics to file if enabled
        if (!cfg.json_output_path.empty()) {
            output_slice_statistics(stats, cfg.json_output_path, engine.output_path());
        }

        if (stats_out && stats_size > 0) {
            size_t n = (json_str.size() < stats_size - 1)
                ? json_str.size() : stats_size - 1;
            memcpy(stats_out, json_str.c_str(), n);
            stats_out[n] = '\0';
        }

        if (!ran) {
            set_error(ctx, stats.error_message.empty()
                ? "Slicing produced no output" : stats.error_message);
        }

        // Flush log before returning (avoid lost entries on abnormal exit)
        Slic3r::flush_logs();

        return engine.exit_code();

    } catch (const std::exception& e) {
        set_error(ctx, std::string("C++ exception: ") + e.what());
        return SLIC3R_ERR_INTERNAL;
    } catch (...) {
        set_error(ctx, "Unknown C++ exception");
        return SLIC3R_ERR_INTERNAL;
    }
}

SLIC3R_API const char* slic3r_get_error(slic3r_ctx_t* ctx) {
    return ctx ? ctx->last_error.c_str() : "NULL context";
}

SLIC3R_API const char* slic3r_version(void) {
    return ENGINE_VERSION;
}

SLIC3R_API void slic3r_cancel(slic3r_ctx_t* ctx) {
    if (ctx) ctx->cancel_requested = true;
}

SLIC3R_API int slic3r_is_cancelled(slic3r_ctx_t* ctx) {
    return ctx ? ctx->cancel_requested.load() : 0;
}
