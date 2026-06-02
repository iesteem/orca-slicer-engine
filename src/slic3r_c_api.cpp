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

#include "Types.hpp"
#include "SliceEngine.hpp"

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

// Parse CLI params JSON into EngineConfig
static bool parse_params(const char* json_str, EngineConfig& cfg) {
    if (!json_str || !json_str[0]) return true; // empty = defaults

    try {
        json j = json::parse(json_str);
        if (j.contains("plate_id"))       cfg.plate_id       = j["plate_id"].get<int>();
        if (j.contains("timeout_seconds")) cfg.timeout_seconds = j["timeout_seconds"].get<int>();
        if (j.contains("max_size_mb"))     cfg.max_size_mb   = j["max_size_mb"].get<int>();
        if (j.contains("substitute_filaments"))    cfg.substitute_filaments    = j["substitute_filaments"].get<bool>();
        if (j.contains("substitute_printer"))     cfg.substitute_printer     = j["substitute_printer"].get<bool>();
        if (j.contains("format")) {
            std::string fmt = j["format"].get<std::string>();
            cfg.format = (fmt == "gcode") ? OutputFormat::GCODE : OutputFormat::GCODE_3MF;
        }
        if (j.contains("cancel_file"))    cfg.cancel_file    = j["cancel_file"].get<std::string>();
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[slic3r] params parse error: %s\n", e.what());
        return false;
    }
}

// Serialize SliceOutputStats to JSON
static std::string stats_to_json(const SliceOutputStats& s) {
    json j;
    j["success"] = s.success;

    if (!s.error_message.empty())
        j["error_message"] = s.error_message;

    j["plates"] = json::array();
    for (const auto& p : s.plates) {
        json pj;
        pj["index"]        = p.plate_id;
        pj["success"]      = p.success;
        pj["gcode_file"]   = p.gcode_file;
        pj["time"] = {
            {"total",   p.total_time},
            {"prepare", p.prepare_time},
            {"print",   p.print_time}
        };
        pj["filament"] = {
            {"total_m",      p.total_filament_m},
            {"total_g",      p.total_filament_g},
            {"model_m",      p.model_filament_m},
            {"model_g",      p.model_filament_g},
            {"total_cost",   p.total_cost}
        };
        pj["support_used"]       = p.support_used;
        pj["toolpath_outside"]   = p.toolpath_outside;
        pj["plate_count"]        = p.plate_count;
        pj["model_thumbnail"]    = p.model_thumbnail;

        // Issues per plate
        pj["issues"] = json::array();
        for (const auto& iss : p.issues) {
            pj["issues"].push_back({
                {"level",       iss.level},
                {"plate_id",    iss.plate_id},
                {"object_name", iss.object_name},
                {"z_height",    iss.z_height},
                {"code",        iss.code},
                {"message",     iss.message},
                {"suggestion",  iss.suggestion}
            });
        }

        // Filament details
        pj["filament_details"] = json::array();
        for (const auto& fd : p.filament_details) {
            pj["filament_details"].push_back({
                {"id",    fd.filament_id},
                {"type",  fd.type},
                {"color", fd.color},
                {"used_g", fd.used_g},
                {"used_m", fd.used_m}
            });
        }

        j["plates"].push_back(pj);
    }

    // Global issues
    j["global_issues"] = json::array();
    for (const auto& iss : s.issues) {
        j["global_issues"].push_back({
            {"level", iss.level}, {"plate_id", iss.plate_id},
            {"object_name", iss.object_name}, {"code", iss.code},
            {"message", iss.message}, {"suggestion", iss.suggestion}
        });
    }

    return j.dump();
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

        // Run the full pipeline
        std::vector<std::string> temp_files;
        TempFileGuard temp_guard(temp_files);

        SliceEngine engine(cfg, temp_files);
        bool ran = engine.run();

        // Serialize stats
        const auto& stats = engine.stats();
        std::string json_str = stats_to_json(stats);

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
    return SLIC3R_VERSION;
}

SLIC3R_API void slic3r_cancel(slic3r_ctx_t* ctx) {
    if (ctx) ctx->cancel_requested = true;
}

SLIC3R_API int slic3r_is_cancelled(slic3r_ctx_t* ctx) {
    return ctx ? ctx->cancel_requested.load() : 0;
}
