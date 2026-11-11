/**
 * slic3r_c_api.h — Pure C boundary for libslic3r shared library
 *
 * This header contains NO C++ types, NO STL, NO templates.
 * It is the ABI-stable contract between the slic3r engine and its consumers.
 *
 * SPIKE 001: Minimum Viable C API — "Slice This File"
 */

#pragma once

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C"
{
#endif

// This project only builds the consumer executable (linking a prebuilt
// slic3r.dll) or the ORCA_STATIC single binary; it no longer compiles
// slic3r.dll from source, so the C API symbols are always imported.
#ifdef _WIN32
#define SLIC3R_API __declspec(dllimport)
#else
#define SLIC3R_API __attribute__((visibility("default")))
#endif

/* ---- Opaque handle ---- */
typedef struct slic3r_ctx_s slic3r_ctx_t;

/* ---- Status codes ----
 * SINGLE SOURCE: Types.hpp defines the C++ EXIT_* constants as aliases of
 * these SLIC3R_* macros, so the C and C++ families cannot drift.
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

    /** Create a slicer context. resources_dir points to a resources root that contains profiles.
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
 *                      WARNING: the report is silently TRUNCATED to
 *                      stats_size-1 bytes; use slic3r_get_last_stats()
 *                      below for a complete, exact-size copy.
 * @param stats_size    Size of stats_out buffer (including null terminator)
 *
 * @return SLIC3R_OK on success, error code on failure.
 *
 * params_json keys (all optional; unknown keys are ignored; validated
 * in slic3r_c_api.cpp parse_params):
 * {
 *   "plate_id": 0,                // 0 = all plates; > 0 rejected (plate
 *                                 // selection removed 2026-11-11 — the engine
 *                                 // always slices all plates, mirroring the
 *                                 // removed CLI -p/--plate)
 *   "format": "gcode.3mf",        // "gcode" | "gcode.3mf"; anything else is rejected
 *   "timeout_seconds": 0,         // >= 0; 0 = no timeout
 *   "max_size_mb": 0,             // >= 0; 0 = no limit. ABSENT key = engine
 *                                 // default 200 MB cap (EngineConfig) — omitting
 *                                 // the key is NOT "no limit"
 *   "cancel_file": "",            // watchdog file: existence triggers cancellation
 *   "log_path": "",               // "" = auto-derive from output path
 *   "json_output_path": "",       // non-empty: report file written on EVERY
 *                                 // engine-executed exit incl. early rc=3/6 (P-28)
 *   "keep_failed_gcode3mf": false // debug hatch: also write gcode.3mf on rc=7
 *                                 // (rc / JSON verdict unchanged)
 * }
 *
 * Report schema summary — authoritative source is
 * docs/requirements/json-output.md, builder is JsonReport.cpp
 * build_statistics_json(), and tests/test_c_api.cpp ("JSON report schema
 * field names") anchors the field names against drift:
 * {
 *   "success": bool,              // == (exit_code == 0) == (error_code empty)
 *   "exit_code": int,             // verbatim process rc; {0,3,4,5,6,7} on
 *                                 // report-emitting paths — rc 1/99 emit NO report
 *   "error_code": "",             // comma-joined code of every error /
 *                                 // serious_warning issue, issue-list order,
 *                                 // NOT deduplicated
 *   "engine_version": "",
 *   "resources_version": "",      // optional; absent = unknown (never a version)
 *   "print_info_total": {
 *     "output_file": "",          // "" on failure
 *     "print_time_seconds": 0.0, "print_time_formatted": "0:00:00",
 *     "total_weight_g": 0.0, "total_length_mm": 0.0, "plate_count": 0,
 *     "filaments": [ { "type","color","used_g","used_mm" } ], // dedup (type,color)
 *     "support_free": bool,       // success runs only; gcode-actual, NOT config flag
 *     "mixed_filament_used": bool // emitted when >= 1 plate was processed
 *   },
 *   "issues": [ {                 // global superset (plate issues funnel in here)
 *     "level": "error|serious_warning|warning|tip",
 *     "plate_id": -1,             // 1-based; -1 = global/not plate-specific
 *     "object_name": "", "z_height": -1.0,
 *     "layer": 0,                 // only emitted when >= 0
 *     "code": "", "message": "",
 *     "suggestion": ""            // only emitted when non-empty
 *   } ],
 *   "plates": [ {
 *     "plate_id": 1,              // 1-based (storage is 0-based, +1 at emission)
 *     "success": bool,
 *     "issues": [ ... ],          // present when non-empty
 *     // success plates additionally carry:
 *     "print_time_seconds": 0.0, "print_time_formatted": "",
 *     "total_weight_g": 0.0,
 *     "filaments": [ { "id": 1, "type": "", "color": "", "used_g": 0.0,
 *                      "used_mm": 0.0 } ],               // id 1-based
 *     "model_thumbnail": "", "long_retraction_when_cut": false,
 *     "support_free": bool        // gcode-actual, inverted at emission
 *   } ]
 * }
 *
 * (This block replaced a pre-2.2.0 draft on 2026-11-06 — it described a
 * schema that never shipped: plates[].index / time.total / prepare /
 * filament.total_m / cost / gcode_file / issue fields "object" and
 * "msg", none of which exist.)
 */
    SLIC3R_API int slic3r_slice(slic3r_ctx_t* ctx, const char* input_3mf, const char* output_base,
                                const char* params_json, char* stats_out, size_t stats_size);

    /* ---- Statistics retrieval ---- */

    /** Copy the COMPLETE JSON statistics report produced by the last
     *  slic3r_slice() call on this ctx (empty string before the first
     *  call). Two-call pattern: probe with out=NULL (or size=0) to get the
     *  required buffer size INCLUDING the NUL terminator, allocate that
     *  much, call again — the full report is copied and the same size is
     *  returned. If size < required, nothing is copied and the required
     *  size is returned (caller retries with a bigger buffer). Returns 0
     *  only when ctx is NULL. Unlike slic3r_slice()'s stats_out buffer,
     *  this never truncates.
     *  Added 2026-11-06 (smell batch 2 E3) — purely additive export. */
    SLIC3R_API size_t slic3r_get_last_stats(slic3r_ctx_t* ctx, char* out, size_t size);

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

