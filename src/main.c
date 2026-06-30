/**
 * orca-slice-engine — pure C consumer of slic3r.dll
 *
 * This file contains NO C++ and NO libslic3r types.
 * All slicing logic lives inside slic3r.dll, accessed via slic3r_c_api.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slic3r_c_api.h"

int main(int argc, char* argv[]) {
    const char* input_3mf  = NULL;
    const char* output     = NULL;
    const char* resources  = NULL;
    const char* log_file   = NULL;
    int         log_enabled = 0;
    int         plate_id   = 0;
    const char* format     = "gcode.3mf";
    int         verbose    = 0;

    /* Parse CLI arguments */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("orca-slice-engine v%s\n", slic3r_version());
            printf("Usage: %s <input.3mf> [options]\n", argv[0]);
            printf("  -o, --output <path>   Output path (without extension)\n");
            printf("  -p, --plate <id>      Plate ID (0=all, default: 0)\n");
            printf("  -f, --format <fmt>    gcode | gcode.3mf (default: gcode.3mf)\n");
            printf("  -r, --resources <dir> Resources directory\n");
            printf("  --log                 Enable log file output\n");
            printf("  --log-file <file>     Specify log file path (implies --log)\n");
            printf("  -v, --verbose         Verbose output\n");
            return 0;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if (++i < argc) output = argv[i];
        } else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--plate")) {
            if (++i < argc) plate_id = atoi(argv[i]);
        } else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--format")) {
            if (++i < argc) format = argv[i];
        } else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--resources")) {
            if (++i < argc) resources = argv[i];
        } else if (!strcmp(argv[i], "--log-file")) {
            if (++i < argc) { log_file = argv[i]; log_enabled = 1; }
        } else if (!strcmp(argv[i], "--log")) {
            /* --log without --log-file: auto-generate log path later */
            verbose = 1;
            log_enabled = 1;
        } else if (argv[i][0] != '-') {
            /* Only accept as input file if it has a .3mf extension
             * (prevents --log-file <path> value from being eaten as input) */
            const char* dot = strrchr(argv[i], '.');
            if (dot && dot[1] == '3'
                && (dot[2] == 'm' || dot[2] == 'M')
                && (dot[3] == 'f' || dot[3] == 'F')
                && dot[4] == '\0')
                input_3mf = argv[i];
        }
    }

    if (!input_3mf) {
        fprintf(stderr, "Error: input file required. Use -h for help.\n");
        return 1;
    }

    /* Default output path from input filename */
    char output_default[1024];
    if (!output) {
        const char* base = strrchr(input_3mf, '/');
        if (!base) base = strrchr(input_3mf, '\\');
        base = base ? base + 1 : input_3mf;
        snprintf(output_default, sizeof(output_default), "%s", base);
        char* dot = strrchr(output_default, '.');
        if (dot) *dot = '\0';
        output = output_default;
    }

    /* Auto-generate log file path when --log is given without --log-file.
     * Log lands beside the output file, e.g. -o /tmp/foo -> /tmp/foo.log */
    char log_auto_path[1024] = {0};
    if (log_enabled && !log_file) {
        snprintf(log_auto_path, sizeof(log_auto_path), "%s.log", output);
        log_file = log_auto_path;
    }

    /* Default resources from environment */
    if (!resources) {
        resources = getenv("ORCA_RESOURCES");
        if (!resources) {
            fprintf(stderr, "Error: --resources required (or set ORCA_RESOURCES)\n");
            return 1;
        }
    }

    if (verbose) printf("orca-slice-engine v%s\n", slic3r_version());

    /* Create slicer context */
    slic3r_ctx_t* ctx = slic3r_create(resources);
    if (!ctx) {
        fprintf(stderr, "FATAL: Failed to initialize slicer\n");
        return 99;
    }

    /* Build params JSON */
    char params[1024];
    if (log_file && log_file[0]) {
        snprintf(params, sizeof(params),
            "{\"plate_id\":%d,\"format\":\"%s\",\"log_path\":\"%s\"}",
            plate_id, format, log_file);
    } else {
        snprintf(params, sizeof(params),
            "{\"plate_id\":%d,\"format\":\"%s\"}", plate_id, format);
    }

    /* Slice */
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, input_3mf, output, params, stats, sizeof(stats));

    if (rc != SLIC3R_OK) {
        fprintf(stderr, "Slice failed (code %d): %s\n", rc, slic3r_get_error(ctx));
        slic3r_destroy(ctx);
        return rc;
    }

    if (verbose && stats[0]) {
        printf("Stats: %s\n", stats);
    }

    slic3r_destroy(ctx);
    return 0;
}
