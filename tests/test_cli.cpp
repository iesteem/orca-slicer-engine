/**
 * @file test_cli.cpp
 * @brief Integration tests driving the BUILT orca-slice-engine executable
 * (main.c consumer layer), covering CLI-layer defects from the 2026-09-16
 * review that unit tests cannot reach:
 *   - (N1) params snprintf accumulation overflow (stack OOB write) — long
 *          paths produced invalid/underminated params JSON
 *   - (C)  no JSON string escaping — Windows-style backslash paths
 *          ("\o" etc.) made the params JSON unparseable
 *   - (C)  atoi() accepted garbage silently ("-p abc" == 0) and negatives
 *   - (C)  "-f bogus" silently mapped to gcode.3mf
 *   - #18  "-p" plate selection was ignored end-to-end (C API wiring)
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
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
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

    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
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

TEST_CASE("CLI: non-numeric plate id rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
                          + "\" -o /tmp/orca_cli_v -p abc");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);   // "Invalid ..." message
}

TEST_CASE("CLI: non-numeric timeout rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
                          + "\" -o /tmp/orca_cli_v -t 10x");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);
}

TEST_CASE("CLI: negative timeout rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
                          + "\" -o /tmp/orca_cli_v -t -5");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);
}

TEST_CASE("CLI: negative max-size rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
                          + "\" -o /tmp/orca_cli_v --max-size -3");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("nvalid") != std::string::npos);
}

TEST_CASE("CLI: unknown -f format rejected", "[integration][cli][validation]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE + "\" -r \"" ORCA_TEST_RESOURCES
                          + "\" -o /tmp/orca_cli_v -f bogus");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.out.find("format") != std::string::npos);
}

// ============================================================================
// #18 "-p" end-to-end — the published exe's plate selection must reach the
// engine: exactly one plate sliced, one .gcode written, no .gcode.3mf.
// Pre-fix, single_plate was never set by the C API: all 4 plates were sliced
// into a .gcode.3mf package.
// ============================================================================

TEST_CASE("CLI: -p 1 -f gcode slices one plate into one .gcode", "[integration][cli][single_plate]")
{
    CliResult r = run_cli(std::string("\"") + ORCA_TEST_FIXTURE_DIR "/filement_count/distinct_per_plate.3mf\""
                          " -r \"" ORCA_TEST_RESOURCES "\" -o /tmp/orca_cli_p1 -p 1 -f gcode");
    INFO(r.out);
    REQUIRE(r.exited);
    REQUIRE(r.exit_code == 0);

    REQUIRE(boost::filesystem::exists("/tmp/orca_cli_p1.gcode"));
    REQUIRE(boost::filesystem::file_size("/tmp/orca_cli_p1.gcode") > 1000);
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_cli_p1.gcode.3mf"));

    // The stats JSON must report exactly the requested plate.
    std::ifstream f("/tmp/orca_cli_p1.json");
    REQUIRE(f.good());
    auto j = nlohmann::json::parse(f);
    REQUIRE(j["plates"].size() == 1);
    REQUIRE(j["plates"][0]["index"].get<int>() == 1);

    rm("/tmp/orca_cli_p1.gcode");
    rm("/tmp/orca_cli_p1.json");
    rm("/tmp/orca_cli_p1.log");
    rm("/tmp/orca_cli_capture.txt");
}
