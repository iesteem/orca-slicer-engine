/**
 * slic3r_c_api.cpp — Production C API boundary for libslic3r shared library.
 *
 * Internally uses the full SliceEngine pipeline. Externally exposes only C.
 * All C++ exceptions are caught at the boundary and converted to error codes.
 */

#include "slic3r_c_api.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Internal C++ includes (hidden from API consumers)
#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "nlohmann/json.hpp"
#include <boost/filesystem.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>

#include "Types.hpp"
#include "Utils.hpp"
#include "SliceEngine.hpp"
#include "JsonReport.hpp"

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

static void set_error(slic3r_ctx_t* ctx, const std::string& msg)
{
    if (ctx)
        ctx->last_error = msg;
    std::cerr << "[slic3r] ERROR: " << msg << std::endl;
}

// Parse CLI params JSON into EngineConfig
static bool parse_params(const char* json_str, EngineConfig& cfg)
{
    if (!json_str || !json_str[0]) return true; // empty = defaults

    try {
        json j = json::parse(json_str);
        if (j.contains("plate_id"))       cfg.plate_id       = j["plate_id"].get<int>();
        if (j.contains("single_plate"))   cfg.single_plate   = j["single_plate"].get<bool>();
        if (j.contains("timeout_seconds")) cfg.timeout_seconds = j["timeout_seconds"].get<int>();
        if (j.contains("max_size_mb"))     cfg.max_size_mb   = j["max_size_mb"].get<int>();
        if (j.contains("substitute_filaments"))    cfg.substitute_filaments    = j["substitute_filaments"].get<bool>();
        if (j.contains("substitute_printer"))     cfg.substitute_printer     = j["substitute_printer"].get<bool>();
        if (j.contains("format")) {
            std::string fmt = j["format"].get<std::string>();
            cfg.format = (fmt == "gcode") ? OutputFormat::GCODE : OutputFormat::GCODE_3MF;
        }
        if (j.contains("cancel_file"))    cfg.cancel_file    = j["cancel_file"].get<std::string>();
        if (j.contains("thread_count"))  cfg.thread_count  = j["thread_count"].get<int>();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[slic3r] params parse error: " << e.what() << std::endl;
        return false;
    }
}

// ====================================================================
// API Implementation
// ====================================================================

SLIC3R_API slic3r_ctx_t* slic3r_create(const char* resources_dir) {
    auto ctx = new (std::nothrow) slic3r_ctx_t();
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

SLIC3R_API void slic3r_enable_file_log(const char* path) {
    if (!path || !path[0]) return;

    namespace expr = boost::log::expressions;
    boost::log::add_common_attributes();

    // Ensure all severity levels are captured
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::trace);

    std::string lpath = path;
    boost::filesystem::path log_parent = boost::filesystem::path(lpath).parent_path();
    if (!log_parent.empty() && !boost::filesystem::exists(log_parent))
        boost::filesystem::create_directories(log_parent);
    boost::log::add_file_log(
        boost::log::keywords::file_name = lpath,
        boost::log::keywords::auto_flush = true,
        boost::log::keywords::format = (
            expr::stream
                << "[" << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S")
                << "] [" << boost::log::trivial::severity << "] " << expr::smessage
        )
    );
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
        set_error(ctx, "Null argument");
        return SLIC3R_ERR_ARGS;
    }

    // Reset cancellation
    ctx->cancel_requested = false;

    try {
        // Build EngineConfig
        EngineConfig cfg;
        cfg.input_file = input_3mf;
        cfg.output_base = output_base;

        // Auto-generate temp directory (same as original main.cpp)
        boost::filesystem::path unique_dir =
            boost::filesystem::temp_directory_path()
            / boost::filesystem::unique_path(
                "orca_slice_%%%%-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(unique_dir);
        cfg.temp_dir = unique_dir.string();
        Slic3r::set_temporary_dir(cfg.temp_dir);

        // Create guard BEFORE parse_params so temp dir is cleaned up
        // on early return from parse failure.
        std::vector<std::string> temp_files;
        TempFileGuard temp_guard(temp_files);

        if (!parse_params(params_json, cfg)) {
            set_error(ctx, "Failed to parse params JSON");
            return SLIC3R_ERR_ARGS;
        }

        // Run the full pipeline

        SliceEngine engine(cfg, temp_files);
        bool ran = engine.run();

        // Output statistics using the same formatter as standalone mode
        const auto& stats = engine.stats();
        std::string json_path = std::string(output_base) + ".json";
        std::string output_path = engine.output_path();
        output_slice_statistics(stats, json_path, output_path);

        // Build compact JSON for stats_out buffer
        std::ifstream ifs(json_path);
        std::string json_str;
        if (ifs.is_open()) {
            json_str.assign((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        }

        if (stats_out && stats_size > 0) {
            size_t n = (json_str.size() < stats_size - 1)
                ? json_str.size() : stats_size - 1;
            std::copy_n(json_str.c_str(), n, stats_out);
            stats_out[n] = '\0';
        }

        if (!ran) {
            set_error(ctx, stats.error_message.empty()
                ? "Slicing produced no output" : stats.error_message);
        }

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
    return ctx ? ctx->last_error.c_str() : "Null context";
}

SLIC3R_API const char* slic3r_version(void) {
    return CLOUD_SLICER_ENGINE_VERSION;
}

SLIC3R_API char* slic3r_get_config_schema(void)
{
    std::string schema = dump_config_schema(Slic3r::print_config_def);
    // malloc/free required here: the C ABI boundary cannot use new/delete
    // because the caller (possibly pure C) has no access to C++ deallocation.
    char* result = static_cast<char*>(malloc(schema.size() + 1));
    if (result) {
        memcpy(result, schema.c_str(), schema.size() + 1);
    }
    return result;
}

SLIC3R_API void slic3r_free_string(char* str)
{
    // Matches slic3r_get_config_schema: C ABI boundary requires malloc/free.
    free(str);
}

SLIC3R_API void slic3r_cancel(slic3r_ctx_t* ctx) {
    if (ctx) ctx->cancel_requested = true;
}

SLIC3R_API int slic3r_is_cancelled(slic3r_ctx_t* ctx) {
    return ctx ? ctx->cancel_requested.load() : 0;
}
