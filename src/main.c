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

int main(int argc, char* argv[])
{
    const char* input_3mf   = NULL;
    const char* output      = NULL;
    const char* resources   = NULL;
    const char* log_file    = NULL;
    const char* json_output = NULL;
    int         plate_id    = 0;
    const char* format      = "gcode.3mf";
    int         verbose     = 0;
    int         timeout_sec = 0;
    int         max_size_mb = 0;
    const char* cancel_file = NULL;
    int         skip_preset_substitution = 0;

    /* Parse CLI arguments */
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            printf("orca-slice-engine v%s\n", slic3r_version());
            printf("Usage: %s <input.3mf> [options]\n", argv[0]);
            printf("  -o, --output <path>   Output path (without extension)\n");
            printf("  -p, --plate <id>      Plate ID (0=all, default: 0)\n");
            printf("  -f, --format <fmt>    gcode | gcode.3mf (default: gcode.3mf)\n");
            printf("  -r, --resources <dir> Resources directory\n");
            printf("  --log [file]         Log file path (without .log extension)\n");
            printf("                        Default: auto-derived from output path\n");
            printf("  -j, --json [file]    JSON statistics output path (without .json extension)\n");
            printf("                        Default: auto-derived from output path\n");
            printf("  -t, --timeout <sec>   Slicing timeout in seconds (0 = no limit)\n");
            printf("  --max-size <mb>       Max input file size in MB (0 = no limit)\n");
            printf("  --cancel-file <file>  Watchdog file for external cancellation\n");
            printf("  -v, --verbose         Verbose output\n");
            printf("  --skip-preset-substitution\n");
            printf("                        Skip official preset enforcement\n");
            return 0;
        }
        else if (!strcmp(argv[i], "--skip-preset-substitution"))
        {
            skip_preset_substitution = 1;
        }
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
        {
            verbose = 1;
        }
        else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--timeout"))
        {
            if (++i < argc) timeout_sec = atoi(argv[i]);
        }
        else if (!strcmp(argv[i], "--max-size"))
        {
            if (++i < argc) max_size_mb = atoi(argv[i]);
        }
        else if (!strcmp(argv[i], "--cancel-file"))
        {
            if (++i < argc) cancel_file = argv[i];
        }
        else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output"))
        {
            if (++i < argc) output = argv[i];
        }
        else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--plate"))
        {
            if (++i < argc) plate_id = atoi(argv[i]);
        }
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--format"))
        {
            if (++i < argc) format = argv[i];
        }
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--resources"))
        {
            if (++i < argc) resources = argv[i];
        }
        else if (!strcmp(argv[i], "--log"))
        {
            /* --log [path]: optional custom log path (without .log) */
            if (i + 1 < argc && argv[i+1][0] != '-')
                log_file = argv[++i];
        }
        else if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--json"))
        {
            if (i + 1 < argc && argv[i+1][0] != '-')
                json_output = argv[++i];
            else
                json_output = "";  // flag present, no value → auto-derive
        }
        else if (argv[i][0] != '-')
        {
            /* Only accept as input file if it has a .3mf extension */
            const char* dot = strrchr(argv[i], '.');
            if (dot && dot[1] == '3'
                && (dot[2] == 'm' || dot[2] == 'M')
                && (dot[3] == 'f' || dot[3] == 'F')
                && dot[4] == '\0')
                input_3mf = argv[i];
        }
    }

    if (!input_3mf)
    {
        fprintf(stderr, "Error: input file required. Use -h for help.\n");
        return 1;
    }

    /* Default output path from input filename */
    char output_default[1024];
    if (!output)
    {
        const char* base = strrchr(input_3mf, '/');
        if (!base) base = strrchr(input_3mf, '\\');
        base = base ? base + 1 : input_3mf;
        snprintf(output_default, sizeof(output_default), "%s", base);
        char* dot = strrchr(output_default, '.');
        if (dot) *dot = '\0';
        output = output_default;
    }

    /* Log and JSON files are always generated.
     * Paths default to <output>.log and <output>.json when not specified.
     * The slic3r_slice engine also auto-derives these internally, but we
     * pass them explicitly so the caller (main.c) retains control. */
    char log_auto_path[1060] = {0};
    if (!log_file)
    {
        int n = snprintf(log_auto_path, sizeof(log_auto_path), "%s.log", output);
        if (n < 0 || (size_t)n >= sizeof(log_auto_path))
            log_auto_path[sizeof(log_auto_path) - 1] = '\0';
        log_file = log_auto_path;
    }

    char json_auto_path[1060] = {0};
    if (!json_output)
    {
        int n = snprintf(json_auto_path, sizeof(json_auto_path), "%s.json", output);
        if (n < 0 || (size_t)n >= sizeof(json_auto_path))
            json_auto_path[sizeof(json_auto_path) - 1] = '\0';
        json_output = json_auto_path;
    }

    /* Default resources from environment */
    if (!resources)
    {
        resources = getenv("ORCA_RESOURCES");
        if (!resources)
        {
            fprintf(stderr, "Error: --resources required (or set ORCA_RESOURCES)\n");
            return 1;
        }
    }

    if (verbose) printf("orca-slice-engine v%s\n", slic3r_version());

    /* Create slicer context */
    slic3r_ctx_t* ctx = slic3r_create(resources);
    if (!ctx)
    {
        fprintf(stderr, "FATAL: Failed to initialize slicer\n");
        return 99;
    }

    /* Build params JSON (incremental to avoid truncation with optional fields) */
    char params[2048];
    int pos = snprintf(params, sizeof(params),
        "{\"plate_id\":%d,\"format\":\"%s\"", plate_id, format);
    pos += snprintf(params + pos, sizeof(params) - pos,
        ",\"log_path\":\"%s\"", log_file ? log_file : "");
    pos += snprintf(params + pos, sizeof(params) - pos,
        ",\"json_output_path\":\"%s\"", json_output ? json_output : "");
    if (timeout_sec > 0)
        pos += snprintf(params + pos, sizeof(params) - pos,
            ",\"timeout_seconds\":%d", timeout_sec);
    if (max_size_mb > 0)
        pos += snprintf(params + pos, sizeof(params) - pos,
            ",\"max_size_mb\":%d", max_size_mb);
    if (cancel_file && cancel_file[0])
        pos += snprintf(params + pos, sizeof(params) - pos,
            ",\"cancel_file\":\"%s\"", cancel_file);
    if (skip_preset_substitution)
        pos += snprintf(params + pos, sizeof(params) - pos,
            ",\"skip_preset_substitution\":true");
    if (pos < (int)sizeof(params))
        snprintf(params + pos, sizeof(params) - pos, "}");

    /* Slice */
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, input_3mf, output, params, stats, sizeof(stats));

    if (rc != SLIC3R_OK)
    {
        fprintf(stderr, "Slice failed (code %d): %s\n", rc, slic3r_get_error(ctx));
        slic3r_destroy(ctx);
        return rc;
    }

    if (verbose && stats[0])
    {
        printf("Stats: %s\n", stats);
    }

    slic3r_destroy(ctx);
    return 0;
}
