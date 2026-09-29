/**
 * @file test_c_api.cpp
 * @brief Integration tests for the production C API boundary
 * (slic3r_c_api.h) — the layer the published CLI exe actually drives.
 *
 * Covers defects found in the 2026-09-16 full review:
 *   - #18  single_plate never wired: plate_id in params_json was ignored
 *   - (N3) missing presets must fail cleanly, never SIGSEGV (P-09); the
 *          deprecated skip_preset_substitution JSON key must be ignored
 *   - (C)  parse_params accepted negative plate_id / timeout / max_size_mb
 *          and silently mapped unknown format to gcode.3mf
 *   - (C)  add_log_file_sink accumulated one sink per slic3r_slice call
 *   - (N2) 1..7 byte "PNG" file passed the size guard and was read OOB
 *          (observed via the decode guard's log message)
 *
 * NOTE on global state: slic3r_create() sets Slic3r::data_dir/resources_dir
 * process-globally. Every case here restores them to ORCA_TEST_RESOURCES on
 * exit so later test files in this binary are unaffected.
 * Tag: [integration][capi].
 */

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Utils.hpp"

#include "slic3r_c_api.h"

static const char* kResources = ORCA_TEST_RESOURCES;
// slic3r_create() expects the resources ROOT (it appends "/profiles"
// itself); kResources is already the profiles dir, so keep the two distinct.
static const char* kResourcesRoot = ORCA_TEST_RESOURCES_ROOT;

namespace {

void rm(const std::string& p)
{
    boost::system::error_code ec;
    boost::filesystem::remove(p, ec);
}

std::string read_file(const std::string& p)
{
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Restore the process-global dirs other test files in this binary expect.
void restore_dirs()
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);
}

uintmax_t file_size_or_zero(const std::string& p)
{
    boost::system::error_code ec;
    uintmax_t sz = boost::filesystem::file_size(p, ec);
    return ec ? 0 : sz;
}

} // namespace

// ============================================================================
// #18 single_plate wiring — plate_id > 0 in params_json must select exactly
// one plate (engine-internal single_plate=true) and, with format "gcode",
// write a single .gcode file instead of packaging every plate into one
// .gcode.3mf. distinct_per_plate.3mf has 4 plates, so plate 1 genuinely
// differs from all-plates mode.
// ============================================================================

TEST_CASE("C API: plate_id=1 + format=gcode slices one plate into a .gcode file", "[integration][capi][single_plate]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    const char* params = R"({"plate_id":1,"format":"gcode","log_path":"/tmp/orca_capi_p1.log"})";
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/filement_count/distinct_per_plate.3mf",
                          "/tmp/orca_capi_p1", params, stats, sizeof(stats));

    REQUIRE(rc == SLIC3R_OK);

    // Exactly one plate in the stats output.
    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["plates"].size() == 1);
    REQUIRE(j["plates"][0]["plate_id"].get<int>() == 1);

    // A .gcode file was written; no all-plates .gcode.3mf package.
    REQUIRE(boost::filesystem::exists("/tmp/orca_capi_p1.gcode"));
    REQUIRE(boost::filesystem::file_size("/tmp/orca_capi_p1.gcode") > 1000);
    REQUIRE_FALSE(boost::filesystem::exists("/tmp/orca_capi_p1.gcode.3mf"));

    rm("/tmp/orca_capi_p1.gcode");
    rm("/tmp/orca_capi_p1.json");
    rm("/tmp/orca_capi_p1.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: plate_id=2 selects plate 2, not plate 1", "[integration][capi][single_plate]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    const char* params = R"({"plate_id":2,"format":"gcode","log_path":"/tmp/orca_capi_p2.log"})";
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/filement_count/distinct_per_plate.3mf",
                          "/tmp/orca_capi_p2", params, stats, sizeof(stats));

    REQUIRE(rc == SLIC3R_OK);
    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["plates"].size() == 1);
    REQUIRE(j["plates"][0]["plate_id"].get<int>() == 2);

    rm("/tmp/orca_capi_p2.gcode");
    rm("/tmp/orca_capi_p2.json");
    rm("/tmp/orca_capi_p2.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

// ============================================================================
// (N3) A missing-preset load must fail cleanly with PRESETS_MISSING (exit 6)
// instead of crashing in init_print (P-09: gdb-confirmed SIGSEGV chain, back
// when --skip-preset-substitution bypassed the load check). The params JSON
// still carries the now-removed skip_preset_substitution key on purpose:
// parse_params must silently ignore unknown keys so callers that used the
// deprecated flag keep working (no crash, no args error).
// ============================================================================

TEST_CASE("C API: missing presets fails cleanly; deprecated skip_preset_substitution key ignored (P-09)", "[integration][capi][presets]")
{
    const std::string empty_res = "/tmp/orca_capi_empty_res";
    boost::system::error_code ec;
    boost::filesystem::create_directories(empty_res, ec); // stays empty: no vendor JSONs

    slic3r_ctx_t* ctx = slic3r_create(empty_res.c_str());
    REQUIRE(ctx != nullptr);

    const char* params = R"({"skip_preset_substitution":true,"log_path":"/tmp/orca_capi_p09.log"})";
    char stats[32768] = {0};
    // If the P-09 defect were present this call SIGSEGVs and the whole test
    // binary dies; the REQUIREs below simply never run. Surviving to assert
    // rc==6 is the regression contract. An unknown JSON key that is NOT
    // silently ignored would surface as SLIC3R_ERR_ARGS here.
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_p09", params, stats, sizeof(stats));

    REQUIRE(rc == SLIC3R_ERR_PREPROCESS);
    REQUIRE(std::string(slic3r_get_error(ctx)).find("presets") != std::string::npos);

    // The stats JSON must also report PRESETS_MISSING.
    auto j = nlohmann::json::parse(stats);
    bool found = false;
    for (const auto& iss : j["issues"])
        if (iss.value("code", "") == "PRESETS_MISSING")
            found = true;
    REQUIRE(found);

    rm("/tmp/orca_capi_p09.json");
    rm("/tmp/orca_capi_p09.log");
    slic3r_destroy(ctx);
    boost::filesystem::remove_all(empty_res, ec);
    restore_dirs();
}

// ============================================================================
// (C) parse_params input validation — negative values and unknown formats
// must be rejected with SLIC3R_ERR_ARGS and a message naming the field,
// not silently coerced.
// ============================================================================

TEST_CASE("C API: negative plate_id rejected as args error", "[integration][capi][validation]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_v1",
                          R"({"plate_id":-1})", nullptr, 0);
    REQUIRE(rc == SLIC3R_ERR_ARGS);
    REQUIRE(std::string(slic3r_get_error(ctx)).find("plate_id") != std::string::npos);

    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: unknown format rejected as args error", "[integration][capi][validation]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_v2",
                          R"({"format":"bogus"})", nullptr, 0);
    REQUIRE(rc == SLIC3R_ERR_ARGS);
    REQUIRE(std::string(slic3r_get_error(ctx)).find("format") != std::string::npos);

    // The legal values must still be accepted (smoke: parse succeeds).
    REQUIRE(slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_v2",
                         R"({"format":"gcode"})", nullptr, 0) != SLIC3R_ERR_ARGS);

    rm("/tmp/orca_capi_v2.gcode.3mf");
    rm("/tmp/orca_capi_v2.gcode");
    rm("/tmp/orca_capi_v2.json");
    rm("/tmp/orca_capi_v2.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: negative timeout_seconds and max_size_mb rejected as args error", "[integration][capi][validation]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    REQUIRE(slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_v3",
                         R"({"timeout_seconds":-5})", nullptr, 0) == SLIC3R_ERR_ARGS);
    REQUIRE(std::string(slic3r_get_error(ctx)).find("timeout") != std::string::npos);

    REQUIRE(slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_v3",
                         R"({"max_size_mb":-2})", nullptr, 0) == SLIC3R_ERR_ARGS);
    REQUIRE(std::string(slic3r_get_error(ctx)).find("max_size_mb") != std::string::npos);

    slic3r_destroy(ctx);
    restore_dirs();
}

// ============================================================================
// (N2) A plate thumbnail "PNG" shorter than the 8-byte signature (1..7
// bytes) must be rejected by the SIZE guard — not pass it and be read
// out-of-bounds by the magic comparison. Observable via the decode guard's
// log message in the slice log: "bad size" (post-fix) vs "bad PNG magic"
// (pre-fix, after an OOB heap read of up to 7 bytes past the buffer).
// ============================================================================

TEST_CASE("C API: 3-byte plate thumbnail rejected by size guard, slice still succeeds", "[integration][capi][thumbnail]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    const char* params = R"({"log_path":"/tmp/orca_capi_thumb.log"})";
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/thumbnail_short_png.3mf",
                          "/tmp/orca_capi_thumb", params, stats, sizeof(stats));

    // Thumbnail decode is best-effort: the slice itself must succeed.
    REQUIRE(rc == SLIC3R_OK);

    const std::string log = read_file("/tmp/orca_capi_thumb.log");
    // The short file must be caught by the size guard...
    REQUIRE(log.find("bad size") != std::string::npos);
    // ...NOT by the magic guard, which reads file_bytes[0..7] OOB on a <8 byte
    // buffer.
    REQUIRE(log.find("bad PNG magic") == std::string::npos);

    rm("/tmp/orca_capi_thumb.gcode.3mf");
    rm("/tmp/orca_capi_thumb.json");
    rm("/tmp/orca_capi_thumb.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

// ============================================================================
// (C) add_log_file_sink leak — every slic3r_slice call used to add ANOTHER
// file sink to the global boost::log core. After slicing twice, the first
// slice's log must be frozen: if it grows during the second slice, the old
// sink is still attached (leak confirmed).
// ============================================================================

TEST_CASE("C API: second slice does not grow the first slice's log (sink leak)", "[integration][capi][logging]")
{
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    const char* p1 = R"({"log_path":"/tmp/orca_capi_sink1.log"})";
    REQUIRE(slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_sink1", p1, nullptr, 0) == SLIC3R_OK);
    // libslic3r's async task thread emits trailing records (remove_backup
    // etc.) for a few ms AFTER slic3r_slice returns. Poll until the file is
    // quiet before taking the baseline, otherwise those delayed lines get
    // misread as second-slice leakage.
    uintmax_t size_after_first = file_size_or_zero("/tmp/orca_capi_sink1.log");
    REQUIRE(size_after_first > 0);
    for (int i = 0; i < 40; ++i) // up to ~2s
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uintmax_t now = file_size_or_zero("/tmp/orca_capi_sink1.log");
        if (now == size_after_first) break;
        size_after_first = now;
    }

    const char* p2 = R"({"log_path":"/tmp/orca_capi_sink2.log"})";
    REQUIRE(slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_sink2", p2, nullptr, 0) == SLIC3R_OK);
    REQUIRE(file_size_or_zero("/tmp/orca_capi_sink2.log") > 0);

    // The first log must not receive the second slice's entries.
    REQUIRE(file_size_or_zero("/tmp/orca_capi_sink1.log") == size_after_first);

    for (const char* tag : {"sink1", "sink2"})
    {
        rm(std::string("/tmp/orca_capi_") + tag + ".gcode.3mf");
        rm(std::string("/tmp/orca_capi_") + tag + ".json");
        rm(std::string("/tmp/orca_capi_") + tag + ".log");
    }
    slic3r_destroy(ctx);
    restore_dirs();
}
