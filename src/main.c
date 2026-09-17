/**
 * orca-slice-engine — pure C consumer of slic3r.dll
 *
 * This file contains NO C++ and NO libslic3r types.
 * All slicing logic lives inside slic3r.dll, accessed via slic3r_c_api.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include "slic3r_c_api.h"

/* Parse a non-negative decimal integer option value. Returns 0 and stores
 * the value on success; prints an error naming the option and returns -1 on
 * garbage input (empty string, non-digits, trailing junk, negative values,
 * overflow). Replaces raw atoi(), which silently maps "-p abc" to plate 0. */
static int parse_int_opt(const char* opt, const char* val, int* out)
{
    char* end = NULL;
    if (!val || !val[0] || val[0] == ' ' || val[0] == '\t')
        goto bad;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < 0 || v > INT_MAX)
        goto bad;
    *out = (int)v;
    return 0;
bad:
    fprintf(stderr, "Error: invalid value for %s: '%s'\n", opt, val ? val : "");
    return -1;
}

/* Append formatted text to buf at pos. Returns the new position, or -1 if
 * the result would not fit in cap bytes (including the NUL). Never lets pos
 * pass the end of the buffer — the accumulation idiom it replaces kept
 * adding snprintf's would-have-written length and wrote past the array. */
static int appendf(char* buf, int pos, size_t cap, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - (size_t)pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - (size_t)pos)
        return -1;
    return pos + n;
}

/* Append a JSON string literal (with surrounding quotes and escaping) for
 * value. Returns the new position, or -1 if it does not fit. Paths may
 * contain backslashes (Windows) or quotes — unescaped they produce invalid
 * JSON that the engine then rejects. */
static int append_json_str(char* buf, int pos, size_t cap, const char* value)
{
    if (pos < 0)
        return -1;
    pos = appendf(buf, pos, cap, "\"");
    if (pos < 0)
        return -1;
    for (const char* p = value ? value : ""; *p; ++p)
    {
        char tmp[8];
        const char* rep = NULL;
        switch (*p)
        {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        default:
            if ((unsigned char)*p < 0x20)
            {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)*p);
                rep = tmp;
            }
            break;
        }
        if (rep)
        {
            pos = appendf(buf, pos, cap, "%s", rep);
            if (pos < 0)
                return -1;
        }
        else
        {
            pos = appendf(buf, pos, cap, "%c", *p);
            if (pos < 0)
                return -1;
        }
    }
    return appendf(buf, pos, cap, "\"");
}

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
            if (++i < argc && parse_int_opt(argv[i - 1], argv[i], &timeout_sec) != 0)
                return 1;
        }
        else if (!strcmp(argv[i], "--max-size"))
        {
            if (++i < argc && parse_int_opt(argv[i - 1], argv[i], &max_size_mb) != 0)
                return 1;
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
            if (++i < argc && parse_int_opt(argv[i - 1], argv[i], &plate_id) != 0)
                return 1;
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

    /* Reject unknown formats here instead of letting the engine silently
     * map them to gcode.3mf (param-validation: never silently coerce). */
    if (strcmp(format, "gcode") != 0 && strcmp(format, "gcode.3mf") != 0)
    {
        fprintf(stderr, "Error: invalid value for -f/--format: '%s' (expected \"gcode\" or \"gcode.3mf\")\n", format);
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

    /* Build params JSON incrementally. Every append is bounds-checked and
     * every string is JSON-escaped: overflowing paths used to push the
     * write offset past the end of this buffer (stack OOB write, review
     * N1) and unescaped Windows-style paths produced unparseable JSON. */
    char params[8192];
    int pos = appendf(params, 0, sizeof(params),
        "{\"plate_id\":%d,\"format\":\"%s\"", plate_id, format);
    if (pos >= 0) pos = appendf(params, pos, sizeof(params), ",\"log_path\":");
    if (pos >= 0) pos = append_json_str(params, pos, sizeof(params), log_file);
    if (pos >= 0) pos = appendf(params, pos, sizeof(params), ",\"json_output_path\":");
    if (pos >= 0) pos = append_json_str(params, pos, sizeof(params), json_output);
    if (pos >= 0 && timeout_sec > 0)
        pos = appendf(params, pos, sizeof(params), ",\"timeout_seconds\":%d", timeout_sec);
    if (pos >= 0 && max_size_mb > 0)
        pos = appendf(params, pos, sizeof(params), ",\"max_size_mb\":%d", max_size_mb);
    if (pos >= 0 && cancel_file && cancel_file[0])
    {
        pos = appendf(params, pos, sizeof(params), ",\"cancel_file\":");
        if (pos >= 0) pos = append_json_str(params, pos, sizeof(params), cancel_file);
    }
    if (pos >= 0 && skip_preset_substitution)
        pos = appendf(params, pos, sizeof(params), ",\"skip_preset_substitution\":true");
    if (pos >= 0) pos = appendf(params, pos, sizeof(params), "}");
    if (pos < 0)
    {
        fprintf(stderr, "Error: arguments too long for params buffer\n");
        slic3r_destroy(ctx);
        return 1;
    }

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
