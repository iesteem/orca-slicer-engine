/**
 * slic3r_c_api.h — Pure C boundary for libslic3r shared library
 *
 * This header contains NO C++ types, NO STL, NO templates.
 * It is the ABI-stable contract between libslic3r.dll and its consumers.
 *
 * SPIKE 001: Minimum Viable C API — "Slice This File"
 */

#pragma once

#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #ifdef SLIC3R_DLL_EXPORTS
    #define SLIC3R_API __declspec(dllexport)
  #else
    #define SLIC3R_API __declspec(dllimport)
  #endif
#else
  #define SLIC3R_API __attribute__((visibility("default")))
#endif

/* ---- Opaque handle ---- */
typedef struct slic3r_ctx_s slic3r_ctx_t;

/* ---- Status codes ---- */
#define SLIC3R_OK                  0
#define SLIC3R_ERR_ARGS            1
#define SLIC3R_ERR_FILE_NOT_FOUND  2
#define SLIC3R_ERR_LOAD            3
#define SLIC3R_ERR_SLICING         4
#define SLIC3R_ERR_EXPORT          5
#define SLIC3R_ERR_VALIDATION      6
#define SLIC3R_ERR_POSTPROCESS     7
#define SLIC3R_ERR_INTERNAL        99

/* ---- Lifecycle ---- */

/** Create a slicer context. resources_dir points to OrcaSlicer/resources/.
 *  Returns a null pointer on failure. */
SLIC3R_API slic3r_ctx_t* slic3r_create(const char* resources_dir);

/** Destroy a slicer context and free all resources. */
SLIC3R_API void slic3r_destroy(slic3r_ctx_t* ctx);

/** Enable Boost file logging. Call BEFORE slic3r_create() to capture all messages.
 *  @param path  Log file path, or null/"auto" to auto-derive from output_base. */
SLIC3R_API void slic3r_enable_file_log(const char* path);

/* ---- Core operation ---- */

/**
 * Slice a 3MF file.
 *
 * @param ctx           Slicer context from slic3r_create()
 * @param input_3mf     Path to input .3mf file
 * @param output_base   Output path WITHOUT extension (e.g. "result")
 * @param params_json   JSON parameters (see below). Null = defaults.
 * @param stats_out     Buffer for JSON statistics output. Null = skip.
 * @param stats_size    Size of stats_out buffer (including null terminator)
 *
 * @return SLIC3R_OK on success, error code on failure.
 *
 * params_json format:
 * {
 *   "plate_id": 0,          // 0 = all plates, N = specific plate (1-based)
 *   "format": "gcode.3mf",  // "gcode" or "gcode.3mf"
 *   "timeout_seconds": 0,   // 0 = no timeout
 *   "max_size_mb": 200,     // max input file size, 0 = no limit
 *   "cancel_file": "",      // watchdog file for external cancellation
 *   "thread_count": 0,        // TBB thread count limit (0 = use all cores)
 *   "substitute_printer": true,   // substitute custom printer preset with official
 *   "substitute_filaments": true  // substitute custom filament presets with official
 * }
 *
 * stats_json format (on success):
 * {
 *   "success": true,
 *   "plates": [{
 *     "index": 1,
 *     "time": {"total": 123.4, "prepare": 12.3, "print": 111.1},
 *     "filament": {"total_m": 1.23, "total_g": 4.56, "cost": 0.12},
 *     "gcode_file": "result.gcode.3mf"
 *   }],
 *   "issues": [{"level":"warning","plate_id":1,"object":"cube","code":"X","msg":"..."}]
 * }
 */
SLIC3R_API int slic3r_slice(
    slic3r_ctx_t*       ctx,
    const char*         input_3mf,
    const char*         output_base,
    const char*         params_json,
    char*               stats_out,
    size_t              stats_size
);

/* ---- Error handling ---- */

/** Get the last error message. Valid until next slic3r_* call on this ctx. */
SLIC3R_API const char* slic3r_get_error(slic3r_ctx_t* ctx);

/* ---- Version ---- */

/** Get the libslic3r version string (e.g. "01.10.01.50"). */
SLIC3R_API const char* slic3r_version(void);

/* ---- Config Schema ---- */

/** Dump all config option definitions (key, type, min, max, enum values)
 *  as a JSON string. Useful for preprocessing layers that need to
 *  clamp or strip incompatible config keys before slicing.
 *
 *  Caller must free the returned string with slic3r_free_string(). */
SLIC3R_API char* slic3r_get_config_schema(void);

/** Free a string returned by slic3r_get_config_schema(). */
SLIC3R_API void slic3r_free_string(char* str);

/* ---- Cancellation ---- */

/** Request cancellation of the current slicing operation.
 *  Safe to call from any thread. Non-blocking. */
SLIC3R_API void slic3r_cancel(slic3r_ctx_t* ctx);

/** Check if cancellation has been requested (non-zero = yes). */
SLIC3R_API int slic3r_is_cancelled(slic3r_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

