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

static void print_usage(const char* prog) {
    printf("orca-slice-engine v%s\n", slic3r_version());
    printf("Usage: %s <input.3mf> [options]\n\n", prog);
    printf("Options:\n");
    printf("  -o, --output <path>   Output path (without extension, default: derived from input)\n");
    printf("  -p, --plate <id>      Plate ID (0=all plates, default: 0)\n");
    printf("  -f, --format <fmt>    Output format: gcode | gcode.3mf (default: gcode.3mf)\n");
    printf("  -r, --resources <dir> Resources directory containing printer profiles\n");
    printf("                         If not set, auto-detected from binary location\n");
    printf("                         (../resources/, then ./resources/, then $ORCA_RESOURCES)\n");
    printf("  -t, --timeout <sec>   Slicing timeout in seconds (0 = no limit)\n");
    printf("  --max-size <mb>       Max input file size in MB (default: 200, 0 = no limit)\n");
    printf("  --cancel-file <file>  Watchdog file for external cancellation\n");
    printf("  --allow-custom-presets          Allow both custom printer and filament presets\n");
    printf("  --allow-custom-printer-presets  Allow custom printer presets\n");
    printf("  --allow-custom-filament-presets Allow custom filament presets\n");
    printf("                                  (default: all presets substituted to official)\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  -h, --help            Show this help\n");
}

int main(int argc, char* argv[]) {
    const char* input_3mf   = NULL;
    const char* output      = NULL;
    const char* resources   = NULL;
    int         plate_id    = 0;
    const char* format      = "gcode.3mf";
    int         verbose     = 0;
    int         timeout_sec = 0;
    int         max_size_mb = 200;
    const char* cancel_file = NULL;
    int         substitute_printer   = 1;  /* default: enforce */
    int         substitute_filaments = 1;  /* default: enforce */

    /* Parse CLI arguments */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
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
        } else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--timeout")) && i + 1 < argc) {
            timeout_sec = atoi(argv[++i]);
            if (timeout_sec < 0) timeout_sec = 0;
        } else if (!strcmp(argv[i], "--max-size") && i + 1 < argc) {
            max_size_mb = atoi(argv[++i]);
            if (max_size_mb < 0) max_size_mb = 0;
        } else if (!strcmp(argv[i], "--cancel-file") && i + 1 < argc) {
            cancel_file = argv[++i];
        } else if (!strcmp(argv[i], "--allow-custom-presets")) {
            substitute_printer   = 0;
            substitute_filaments = 0;
        } else if (!strcmp(argv[i], "--allow-custom-printer-presets")) {
            substitute_printer = 0;
        } else if (!strcmp(argv[i], "--allow-custom-filament-presets")) {
            substitute_filaments = 0;
        } else if (argv[i][0] != '-') {
            input_3mf = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
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

    /* Build params JSON with all CLI flags */
    char params[1024];
    char cancel_str[64] = "";
    if (cancel_file) {
        snprintf(cancel_str, sizeof(cancel_str), ",\"cancel_file\":\"%s\"", cancel_file);
    }
    snprintf(params, sizeof(params),
        "{"
        "\"plate_id\":%d,"
        "\"format\":\"%s\","
        "\"timeout_seconds\":%d,"
        "\"max_size_mb\":%d,"
        "\"substitute_printer\":%s,"
        "\"substitute_filaments\":%s"
        "%s"
        "}",
        plate_id, format, timeout_sec, max_size_mb,
        substitute_printer   ? "true" : "false",
        substitute_filaments ? "true" : "false",
        cancel_str);

    /* Slice */
    char stats[32768];
    memset(stats, 0, sizeof(stats));
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
