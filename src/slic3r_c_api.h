/**
 * slic3r_c_api.h — Pure C boundary for libslic3r shared library
 *
 * This header contains NO C++ types, NO STL, NO templates.
 * It is the ABI-stable contract between libslic3r.dll and its consumers.
 *
 * SPIKE 001: Minimum Viable C API — "Slice This File"
 */

#pragma once

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C"
{
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

/* ---- Status codes ----
 * Values 0-7 match the C++ EXIT_* constants in Types.hpp exactly;
 * slic3r_slice() returns engine.exit_code() directly without translation.
 *
 *   0  SLIC3R_OK              Success
 *   1  SLIC3R_ERR_ARGS        Invalid command-line arguments
 *   2  SLIC3R_ERR_FILE_NOT_FOUND  Input file not found
 *   3  SLIC3R_ERR_LOAD        3MF load / parse failure
 *   4  SLIC3R_ERR_SLICING     Slicing failure (geometry defects / timeout / cancellation / internal error)
 *   5  SLIC3R_ERR_EXPORT      Output write failure (GCode I/O error / .3mf packaging error)
 *   6  SLIC3R_ERR_PREPROCESS  Pre-slicing validation error (config / input / presets invalid)
 *   7  SLIC3R_ERR_POSTPROCESS Post-slicing fatal error (GCode generated but unusable, e.g. empty layers / toolpath out of bounds)
 *  99  SLIC3R_ERR_INTERNAL    C++ exception crossing API boundary (unexpected internal error)
 */
#define SLIC3R_OK 0
#define SLIC3R_ERR_ARGS 1
#define SLIC3R_ERR_FILE_NOT_FOUND 2
#define SLIC3R_ERR_LOAD 3
#define SLIC3R_ERR_SLICING 4
#define SLIC3R_ERR_EXPORT 5
#define SLIC3R_ERR_PREPROCESS 6
#define SLIC3R_ERR_POSTPROCESS 7
#define SLIC3R_ERR_INTERNAL 99

    /* ---- Lifecycle ---- */

    /** Create a slicer context. resources_dir points to OrcaSlicer/resources/.
 *  Returns NULL on failure. */
    SLIC3R_API slic3r_ctx_t* slic3r_create(const char* resources_dir);

    /** Destroy a slicer context and free all resources. */
    SLIC3R_API void slic3r_destroy(slic3r_ctx_t* ctx);

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
 *   "plate_id": 0,           // 0 = all plates, N = specific plate (1-based)
 *   "format": "gcode.3mf",   // "gcode" or "gcode.3mf"
 *   "timeout_seconds": 0,    // 0 = no timeout
 *   "max_size_mb": 200,      // max input file size, 0 = no limit
 *   "cancel_file": "",       // watchdog file for external cancellation
 *   "log_path": "",          // log file path (empty = no file logging)
 *   "json_output_path": ""   // JSON statistics output file path
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
 *   "issues": [{"level":"warning","plate_id":1,"object":"cube","code":"X","msg":"..."},
 *              {"level":"serious_warning","plate_id":1,"object":"cube","code":"Y","msg":"..."},
 *              {"level":"error","plate_id":1,"object":"cube","code":"Z","msg":"..."}]
 * }
 */
    SLIC3R_API int slic3r_slice(slic3r_ctx_t* ctx, const char* input_3mf, const char* output_base,
                                const char* params_json, char* stats_out, size_t stats_size);

    /* ---- Error handling ---- */

    /** Get the last error message. Valid until next slic3r_* call on this ctx. */
    SLIC3R_API const char* slic3r_get_error(slic3r_ctx_t* ctx);

    /* ---- Version ---- */

    /** Get the engine version string (e.g. "02.01.01"). */
    SLIC3R_API const char* slic3r_version(void);

    /* ---- Cancellation ---- */

    /** Request cancellation of the current slicing operation.
 *  Safe to call from any thread. Non-blocking. */
    SLIC3R_API void slic3r_cancel(slic3r_ctx_t* ctx);

    /** Check if cancellation has been requested (non-zero = yes). */
    SLIC3R_API int slic3r_is_cancelled(slic3r_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

