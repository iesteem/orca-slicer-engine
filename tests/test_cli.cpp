/**
 * @file test_cli.cpp
 * @brief Integration tests driving the BUILT orca-slice-engine executable
 * (main.c consumer layer), covering CLI-layer defects from the 2026-09-16
 * review that unit tests cannot reach:
 *   - (N1) params snprintf accumulation overflow (stack OOB write) — long
 *          paths produced invalid/underminated params JSON
 *   - (C)  no JSON string escaping — Windows-style backslash paths
 *          ("\o" etc.) made the params JSON unparseable
 *   - (C)  atoi() accepted garbage silently ("-t abc" == 0) and negatives
 *   - (C)  "-f bogus" silently mapped to gcode.3mf
 *   - #18  "-p" plate selection was ignored end-to-end (C API wiring);
 *          the flag itself was removed 2026-09-30 (CLI always slices all
 *          plates; single-plate coverage lives at C API/engine level, and
 *          the removed flag must be rejected — see the deprecated test below)
 *
 * The exe path is injected via ORCA_ENGINE_EXE at build time.
 * Tag: [integration][cli].
 */

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace {

struct CliResult
{
    int status = -1;   // raw wait status from system()
    bool exited = false;
    int exit_code = -1;
    std::string out;   // combined stdout+stderr
};

CliResult run_cli(const std::string& args)
{
    CliResult r;
    const std::string cmd = std::string("\"" ORCA_ENGINE_EXE "\" ") + args + " > /tmp/orca_cli_capture.txt 2>&1";
    r.status = std::system(cmd.c_str());
    if (WIFEXITED(r.status))
    {
        r.exited = true;
        r.exit_code = WEXITSTATUS(r.status);
    }
    std::ifstream f("/tmp/orca_cli_capture.txt");
    r.out = std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return r;
}

void rm(const std::string& p)
{
    boost::system::error_code ec;
    boost::filesystem::remove(p, ec);
}

} // namespace

// ============================================================================
// (C) JSON escaping — a log path containing backslashes (typical Windows
// absolute path) must not corrupt the params JSON. Pre-fix: "\o" is an
// illegal JSON escape → parse_params fails → "Failed to parse params JSON"
// and exit 1. Post-fix the slice proceeds and succeeds.
// ============================================================================

TEST_CASE("CLI: backslash log path survives params JSON (escaping)", "[integration][cli][escaping]")
{
    // Literal backslash in the file name — legal on Linux, fatal to JSON.
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_esc --log '/tmp/orca_cli_esc\\o1'");

    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.out.find("Failed to parse params JSON") == std::string::npos);
    REQUIRE(r.exit_code == 0);

    rm("/tmp/orca_cli_esc.gcode.3mf");
    rm("/tmp/orca_cli_esc.json");
    rm("/tmp/orca_cli_esc.log");
    rm("/tmp/orca_cli_esc\\o1.log");
}

// ============================================================================
// (N1) snprintf accumulation overflow — combined log/json paths longer than
// the params buffer used to push pos past its end, producing an unterminated
// (out-of-bounds-written) buffer and unparseable JSON. Post-fix the params
// JSON is always well-formed; the slice then fails later for honest reasons
// (output dir does not exist), never with a params parse error or a signal.
// ============================================================================

TEST_CASE("CLI: overflowing path lengths keep params JSON well-formed", "[integration][cli][overflow]")
{
    // ~2400 chars of <255-char components: valid shape, but log+json
    // derivations far exceed the 2048-byte params buffer.
    std::string long_path = "/tmp/orca_cli_long/";
    for (int i = 0; i < 14; ++i)
        long_path += std::string(180, 'a') + "/";
    long_path += "out";

    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o '" + long_path + "'");

    INFO(r.out);
    REQUIRE(r.exited);                                        // no stack-smash crash
    REQUIRE(r.out.find("Failed to parse params JSON") == std::string::npos);
    REQUIRE(r.exit_code != 1);                                // honest later failure, not args
}

// ============================================================================
// (C) atoi validation — non-numeric and negative values must be rejected
// with a clear message, not silently coerced to 0 / accepted.
// ============================================================================

TEST_CASE("CLI: non-numeric timeout rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_v -t 10x");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);
}

TEST_CASE("CLI: negative timeout rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_v -t -5");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);
}

TEST_CASE("CLI: negative max-size rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_v --max-size -3");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);
}

TEST_CASE("CLI: unknown -f format rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_v -f bogus");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("format") != std::string::npos);
}

// ============================================================================
// (external MR !3 review F4, fixed 2026-10-20) A required-value option ending
// the command line bare used to be silently swallowed by the "++i < argc"
// guard — the call sliced on defaults instead of erroring, contradicting the
// strict-validation posture (-p explicitly rejected, garbage integers
// rejected). All six option groups now exit 1 naming the exact flag form.
// GENERATE runs each flag as an independent case: one regression must not
// mask the other nine. --log/-j keep their documented optional-value
// semantics — bare --log is pinned below.
// ============================================================================

TEST_CASE("CLI: value-taking option ending command line bare is rejected", "[integration][cli][validation]")
{
    const char* flag = GENERATE("-t", "--timeout", "--max-size", "--cancel-file",
                                "-o", "--output", "-f", "--format", "-r", "--resources");

    // Pre-clean: a stale artifact from an unrelated run must not turn the
    // no-product assertion below into a false failure (test-artifact-stale-trap).
    rm("/tmp/orca_cli_v.gcode.3mf");

    // Full valid context with the bare flag last — the exact caller bug shape.
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_v " + flag);
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find(std::string("option ") + flag + " requires a value") != std::string::npos);
    // The rejection must happen at parse time — never a silent default slice.
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_cli_v.gcode.3mf"));

    rm("/tmp/orca_cli_capture.txt");
}

TEST_CASE("CLI: bare --log keeps optional-value semantics and proceeds", "[integration][cli][validation]")
{
    // --log's value is optional by contract: a bare flag must NOT trip the
    // missing-value rejection the six groups above rely on — it proceeds
    // until an honest later failure (no input file in this invocation).
    CliResult r = run_cli("--log");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("requires a value") == std::string::npos);
    REQUIRE(r.out.find("input file required") != std::string::npos);

    rm("/tmp/orca_cli_capture.txt");
}

// ============================================================================
// Removed -p/--plate flag — removed 2026-09-30. The CLI always slices all
// plates. Unlike other unknown options (silently ignored, see the deprecated
// flag test below), -p is REJECTED: honoring-by-ignoring would silently widen
// a single-plate request to all plates (N x work, different artifact name
// "-p<N>" suffix gone, different exit-code semantics). This is the one
// documented exception to ignore-unknown-options. Plate selection lives on
// via the C API params_json "plate_id" key (test_c_api.cpp) and engine-level
// single_plate (test_slice_e2e.cpp Cases 5/6/16).
// ============================================================================

TEST_CASE("CLI: removed -p/--plate rejected with exit 1", "[integration][cli][deprecated]")
{
    rm("/tmp/orca_cli_rm.gcode.3mf");  // pre-clean: stale leftovers must not satisfy assertions
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_rm -p 2");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("removed") != std::string::npos);
    REQUIRE(r.out.find("all plates") != std::string::npos);
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_cli_rm.gcode.3mf"));

    rm("/tmp/orca_cli_rm.json");
    rm("/tmp/orca_cli_rm.log");
    rm("/tmp/orca_cli_capture.txt");
}

// ============================================================================
// Deprecated --skip-preset-substitution flag — removed 2026-09-29. main.c
// silently ignores unknown options, so callers still passing the flag must get
// a normal slice (exit 0, output produced): the flag is dead weight, never an
// error. Official preset substitution runs as always.
// ============================================================================

TEST_CASE("CLI: deprecated --skip-preset-substitution silently ignored, slice proceeds", "[integration][cli][deprecated]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES_ROOT
                          + "\" -o /tmp/orca_cli_dep --skip-preset-substitution");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 0);
    REQUIRE(boost::filesystem::exists("/tmp/orca_cli_dep.gcode.3mf"));

    rm("/tmp/orca_cli_dep.gcode.3mf");
    rm("/tmp/orca_cli_dep.json");
    rm("/tmp/orca_cli_dep.log");
    rm("/tmp/orca_cli_capture.txt");
}
