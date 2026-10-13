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

#include "Types.hpp" // EXIT_PREPROCESS_ERROR

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

// Read one text entry (e.g. "Metadata/slice_info.config") from the packaged
// .gcode.3mf. libzip is not linked into the test binary; shell out to python3
// (available on every host that builds the engine) — or fail the test loudly
// if either step breaks. Same pattern as test_high_flow.cpp.
std::string extract_3mf_text(const std::string& gcode_3mf_path, const std::string& entry)
{
    const std::string tmp = "/tmp/orca_test_capi_extract_entry.txt";
    boost::filesystem::remove(tmp);
    const std::string cmd =
        "python3 -c \"import zipfile,sys;"
        "z=zipfile.ZipFile('" + gcode_3mf_path + "');"
        "sys.exit(1) if '" + entry + "' not in z.namelist() else "
        "open('" + tmp + "','wb').write(z.read('" + entry + "'))\"";
    const int rc = std::system(cmd.c_str());
    REQUIRE(rc == 0);
    std::ifstream f(tmp, std::ios::binary);
    REQUIRE(f.good());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

// ============================================================================
// #18 single_plate wiring — plate_id > 0 in params_json must select exactly
// one plate (engine-internal single_plate=true) and, with format "gcode",
// write a single .gcode file instead of packaging every plate into one
// .gcode.3mf. distinct_per_plate.3mf has 4 plates, so plate 1 genuinely
// differs from all-plates mode.
// ============================================================================

// ============================================================================
// resources_version — verbatim "version" from <resources>/profiles/Snapmaker.json
// echoed into the stats JSON. Must survive failure exits (that is when the
// resources version matters most for triage), so this drives the cheap
// printer-reject path rather than a full slice. Expected value is read from
// the same file at test time, so the assertion survives resources bumps.
// ============================================================================

TEST_CASE("C API: resources_version echoes vendor profile version on failure exits", "[integration][capi][resources_version]")
{
    // Expected version straight from the test resources' vendor root.
    auto vj = nlohmann::json::parse(read_file(std::string(kResources) + "/Snapmaker.json"));
    const std::string expected = vj.at("version").get<std::string>();
    REQUIRE_FALSE(expected.empty());

    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    // Printer-reject fixture: run() fails fast at validate_printer_model —
    // no slicing — but still serializes stats.
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/printer_reject/bambu_p1s.3mf",
                          "/tmp/orca_capi_resver", R"({"log_path":"/tmp/orca_capi_resver.log"})",
                          stats, sizeof(stats));
    REQUIRE(rc == EXIT_PREPROCESS_ERROR);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j.contains("resources_version"));
    REQUIRE(j["resources_version"].get<std::string>() == expected);

    rm("/tmp/orca_capi_resver.json");
    rm("/tmp/orca_capi_resver.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

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

    // No plate was processed → the load-time mixed_filament_used flag must be
    // absent (a "false" here would let consumers misread "never parsed" as
    // "no mixed filament in use").
    REQUIRE_FALSE(j["print_info_total"].contains("mixed_filament_used"));

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
// support_free JSON contract. The flag is G-CODE ACTUAL: per-plate support
// extrusion volume (GCodeProcessor support_volumes_per_extruder, roles
// erSupportMaterial/Interface/Transition — normal, tree and raft support)
// == 0, inverted at the emission boundary. It is NOT the config-level
// enable_support flag: support enabled on self-supporting geometry extrudes
// zero support and stays "free". Since P-25 (2026-10-10) the packaged 3MF
// plate metadata (Metadata/slice_info.config, key "support_used") carries
// the SAME gcode-actual source — desktop writes the config layer there, but
// that field has zero readers in the fork tree and cloud consumers need the
// actual layer (asserted in cases 2 and 3 below).
//   1. success + no support extruded anywhere -> print_info_total.support_free
//      == true, plates[].support_free == true
//   2. success + enable_support but self-supporting geometry -> still true
//      (config on, zero generation — the config/generation layer boundary;
//      3MF metadata support_used == false here, vs true on desktop)
//   3. success + overhang geometry that really generates support -> both
//      false (and 3MF metadata support_used == true)
//   4. failure -> support_free ABSENT: a failed plate's support state was
//      never determined, so a present-but-true value would let consumers
//      misread "unknown" as "support-free"
// ============================================================================

TEST_CASE("C API: support_free true when no support is extruded", "[integration][capi][support]")
{
    // geom_above_bed_ok.3mf: cube resting on the bed, support not enabled —
    // trivially zero support extrusion.
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/geom_above_bed_ok.3mf",
                          "/tmp/orca_capi_sup_off", R"({"log_path":"/tmp/orca_capi_sup_off.log"})", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_OK);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["success"].get<bool>());
    REQUIRE(j["print_info_total"].contains("support_free"));
    REQUIRE(j["print_info_total"]["support_free"].get<bool>() == true);
    REQUIRE(j["plates"][0]["support_free"].get<bool>() == true);

    rm("/tmp/orca_capi_sup_off.gcode.3mf");
    rm("/tmp/orca_capi_sup_off.json");
    rm("/tmp/orca_capi_sup_off.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: enable_support on self-supporting cube leaves support_free true", "[integration][capi][support]")
{
    // geom_above_bed_support_on.3mf: identical geometry to the case above but
    // the object config carries enable_support=1 (object-level merge path,
    // PrintApply.cpp m_support_used |= object config). The cube is
    // self-supporting: zero support is generated, so the gcode-actual flag
    // stays true — the JSON no longer reflects the config flag. P-25: the
    // packaged 3MF metadata shares this source, so support_used=false here
    // (desktop would write true — config layer; asserted to pin the flip).
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    rm("/tmp/orca_capi_sup_on.gcode.3mf"); // artifact assertion below: pre-clean (stale-path trap)
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/geom_above_bed_support_on.3mf",
                          "/tmp/orca_capi_sup_on", R"({"log_path":"/tmp/orca_capi_sup_on.log"})", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_OK);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["success"].get<bool>());
    REQUIRE(j["print_info_total"]["support_free"].get<bool>() == true);
    REQUIRE(j["plates"][0]["support_free"].get<bool>() == true);

    // P-25 anchor: 3MF plate metadata is gcode-actual, not the config layer.
    // Fails if the relay ever reverts to result.support_used (config).
    const std::string slice_info =
        extract_3mf_text("/tmp/orca_capi_sup_on.gcode.3mf", "Metadata/slice_info.config");
    REQUIRE(slice_info.find("key=\"support_used\" value=\"false\"") != std::string::npos);

    rm("/tmp/orca_capi_sup_on.gcode.3mf");
    rm("/tmp/orca_capi_sup_on.json");
    rm("/tmp/orca_capi_sup_on.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: overhang geometry that generates support flips support_free false", "[integration][capi][support]")
{
    // geom_overhang_support_generated.3mf: T-shape (13.5mm column + 27mm cap,
    // 1mm overlap) with object-level enable_support=1 — the cap underside
    // ring at z=13.5 is a flat overhang below the 30° threshold, so support
    // columns are really extruded and the gcode-actual flag must be false.
    // A floating instance-Z variant is impossible: ensure-on-bed drops
    // single-part objects to the bed (SliceEngine.cpp z_shift = -min_z).
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    rm("/tmp/orca_capi_sup_gen.gcode.3mf"); // artifact assertion below: pre-clean (stale-path trap)
    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/geom_overhang_support_generated.3mf",
                          "/tmp/orca_capi_sup_gen", R"({"log_path":"/tmp/orca_capi_sup_gen.log"})", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_OK);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["success"].get<bool>());
    REQUIRE(j["print_info_total"]["support_free"].get<bool>() == false);
    REQUIRE(j["plates"][0]["support_free"].get<bool>() == false);

    // P-25 anchor: real generation must also relay into the 3MF metadata.
    const std::string slice_info =
        extract_3mf_text("/tmp/orca_capi_sup_gen.gcode.3mf", "Metadata/slice_info.config");
    REQUIRE(slice_info.find("key=\"support_used\" value=\"true\"") != std::string::npos);

    rm("/tmp/orca_capi_sup_gen.gcode.3mf");
    rm("/tmp/orca_capi_sup_gen.json");
    rm("/tmp/orca_capi_sup_gen.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: support_free absent when slice fails", "[integration][capi][support]")
{
    // PRESETS_MISSING fails the run before any plate is sliced; the global
    // flag must be omitted entirely rather than defaulting to a value.
    const std::string empty_res = "/tmp/orca_capi_empty_res_sup";
    boost::system::error_code ec;
    boost::filesystem::create_directories(empty_res, ec);

    slic3r_ctx_t* ctx = slic3r_create(empty_res.c_str());
    REQUIRE(ctx != nullptr);

    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE, "/tmp/orca_capi_sup_fail", "{}", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_ERR_PREPROCESS);

    auto j = nlohmann::json::parse(stats);
    REQUIRE_FALSE(j["success"].get<bool>());
    REQUIRE_FALSE(j["print_info_total"].contains("support_free"));

    rm("/tmp/orca_capi_sup_fail.json");
    slic3r_destroy(ctx);
    boost::filesystem::remove_all(empty_res, ec);
    restore_dirs();
}

TEST_CASE("C API: real-world boat pair + cube pin gcode-actual support verdict", "[integration][capi][support]")
{
    // Production-exported trio (2026-10-13, tests/fixtures/support/): the SAME
    // real-mesh boat saved twice, support off vs on — a controlled pair where
    // only the support setting differs — plus a cube saved with support on.
    // Complements the handcrafted fixtures above: these carry desktop-saved
    // support config through the full load/substitution chain, and the 2.5MB
    // boat mesh exercises real support generation on curved overhangs
    // (~1550 TYPE:Support segments at capture time on the "on" save).
    //   boat_support_off -> free=true,  3MF support_used=false
    //   boat_support_on  -> free=false, 3MF support_used=true
    //   cube_support_on  -> free=true,  3MF support_used=false (config layer
    //                       never participates in the verdict)
    struct Case { const char* fixture; bool expect_free; const char* expect_used; };
    const Case cases[] = {
        {"support/boat_support_off.3mf", true, "false"},
        {"support/boat_support_on.3mf", false, "true"},
        {"support/cube_support_on.3mf", true, "false"},
    };
    for (const auto& c : cases) {
        slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
        REQUIRE(ctx != nullptr);

        std::string name = std::strrchr(c.fixture, '/') + 1; // "boat_support_off.3mf"
        name.resize(name.find('.'));                          // "boat_support_off"
        const std::string stem = "/tmp/orca_capi_sup_real_" + name;
        for (const char* ext : {".gcode.3mf", ".json", ".log"})
            rm(stem + ext); // pre-clean: artifact assertions below (stale-path trap)
        const std::string opts = "{\"log_path\":\"" + stem + ".log\"}";

        char stats[32768] = {0};
        int rc = slic3r_slice(ctx, (std::string(ORCA_TEST_FIXTURE_DIR) + "/" + c.fixture).c_str(),
                              stem.c_str(), opts.c_str(), stats, sizeof(stats));
        REQUIRE(rc == SLIC3R_OK);

        auto j = nlohmann::json::parse(stats);
        REQUIRE(j["success"].get<bool>());
        REQUIRE(j["print_info_total"]["support_free"].get<bool>() == c.expect_free);
        REQUIRE(j["plates"][0]["support_free"].get<bool>() == c.expect_free);

        // P-25 dual-source anchor: the packaged 3MF metadata carries the SAME
        // gcode-actual verdict as the JSON flag.
        const std::string slice_info =
            extract_3mf_text(stem + ".gcode.3mf", "Metadata/slice_info.config");
        REQUIRE(slice_info.find(std::string("key=\"support_used\" value=\"") + c.expect_used + "\"")
                != std::string::npos);

        rm(stem + ".gcode.3mf");
        rm(stem + ".json");
        rm(stem + ".log");
        slic3r_destroy(ctx);
        restore_dirs();
    }
}

// ============================================================================
// mixed_filament_used JSON contract. The flag distinguishes DEFINING mixed
// filaments (mixed_filament_definitions rows in project_settings.config)
// from USING one (a reference to a virtual slot via object/part extruder or
// a painted facet state above the physical filament_colour count). Height-
// range colour overrides are not a usage path: the GUI supports physical
// filaments only there. It is an input-file property determined at load
// time, so — unlike support_free — it is emitted whenever at least one
// plate was processed, including partially failed runs; it is absent only
// on load-stage failures where the model was never parsed.
// The two fixtures share byte-identical definitions (3 enabled rows, 4
// physical filaments → virtual ids 5/6/7); they differ ONLY in usage.
// ============================================================================

TEST_CASE("C API: mixed_filament_used true when a virtual slot is referenced", "[integration][capi][mixed]")
{
    // mixed_filament_used.3mf: objects 4/6/8 carry extruder=5/6/7 (all three
    // virtual slots); slicing must succeed and expand them to physical slots
    // (gcode contains only T0..T3 — verified live 2026-10-08).
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/mixed_filament_used.3mf",
                          "/tmp/orca_capi_mix_used", R"({"log_path":"/tmp/orca_capi_mix_used.log"})", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_OK);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["success"].get<bool>());
    REQUIRE(j["print_info_total"]["mixed_filament_used"].get<bool>() == true);
    // The virtual ids expanded to physical components: more than one slot
    // carries filament (mixed slot 5 = gradient of physical 1/2/4 etc.).
    REQUIRE(j["plates"][0]["filaments"].size() > 1);

    rm("/tmp/orca_capi_mix_used.gcode.3mf");
    rm("/tmp/orca_capi_mix_used.json");
    rm("/tmp/orca_capi_mix_used.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: mixed_filament_used false when defined but never referenced", "[integration][capi][mixed]")
{
    // mixed_filament_defined_unused.3mf: same 3 definitions, every object
    // extruder=0 → definitions are inert; the flag must stay false (defining
    // a mixed filament is not using it).
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/mixed_filament_defined_unused.3mf",
                          "/tmp/orca_capi_mix_unused", R"({"log_path":"/tmp/orca_capi_mix_unused.log"})", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_OK);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["success"].get<bool>());
    REQUIRE(j["print_info_total"]["mixed_filament_used"].get<bool>() == false);

    rm("/tmp/orca_capi_mix_unused.gcode.3mf");
    rm("/tmp/orca_capi_mix_unused.json");
    rm("/tmp/orca_capi_mix_unused.log");
    slic3r_destroy(ctx);
    restore_dirs();
}

TEST_CASE("C API: mixed filament via modifier reference and via painted facets", "[integration][capi][mixed]")
{
    // Two more usage paths on the same 4-physical/3-virtual setup:
    //   modifier fixture — a ParameterModifier part (and its object) carries
    //     extruder=6; the flag must fire on the volume-level config.
    //   painted fixture  — extruder stays 0 everywhere; usage lives only in
    //     the mesh paint_color facet stream (mmu_segmentation used_states,
    //     virtual ids above 4), exercising the facet path of the detector.
    struct Case { const char* fixture; const char* tag; };
    for (const Case& c : {Case{"/mixed_filament_modifier_used.3mf", "mix_mod"},
                          Case{"/mixed_filament_painted_used.3mf", "mix_paint"}})
    {
        INFO("fixture: " << c.fixture);
        slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
        REQUIRE(ctx != nullptr);

        char stats[32768] = {0};
        std::string out = std::string("/tmp/orca_capi_") + c.tag;
        std::string params = std::string(R"({"log_path":")") + out + ".log\"}";
        int rc = slic3r_slice(ctx, (std::string(ORCA_TEST_FIXTURE_DIR) + c.fixture).c_str(),
                              out.c_str(), params.c_str(), stats, sizeof(stats));
        REQUIRE(rc == SLIC3R_OK);

        auto j = nlohmann::json::parse(stats);
        REQUIRE(j["success"].get<bool>());
        REQUIRE(j["print_info_total"]["mixed_filament_used"].get<bool>() == true);
        // The virtual slot expanded into physical components (multiple slots
        // carry filament after component expansion).
        REQUIRE(j["plates"][0]["filaments"].size() > 1);

        rm(out + ".gcode.3mf");
        rm(out + ".json");
        rm(out + ".log");
        slic3r_destroy(ctx);
        restore_dirs();
    }
}

TEST_CASE("C API: mixed_filament_used is OR-aggregated across plates", "[integration][capi][mixed]")
{
    // mixed_filament_two_plate.3mf: mixed_filament_used.3mf derived to a
    // second plate — plate 1 keeps the virtual-slot reference (expands to all
    // 4 physical slots), plate 2 is pure physical (a single slot). The flag is
    // a FILE-level OR over plates: any plate referencing a virtual slot sets it
    // true, and a pure-physical plate does not drag it back down (no AND
    // semantics — plate 2 being physical-only must not clear it).
    slic3r_ctx_t* ctx = slic3r_create(kResourcesRoot);
    REQUIRE(ctx != nullptr);

    char stats[32768] = {0};
    int rc = slic3r_slice(ctx, ORCA_TEST_FIXTURE_DIR "/mixed_filament_two_plate.3mf",
                          "/tmp/orca_capi_mix_twoplate", R"({"log_path":"/tmp/orca_capi_mix_twoplate.log"})", stats, sizeof(stats));
    REQUIRE(rc == SLIC3R_OK);

    auto j = nlohmann::json::parse(stats);
    REQUIRE(j["success"].get<bool>());
    REQUIRE(j["plates"].size() == 2);
    // File-level OR aggregation: plate 1 references virtual slots → true.
    REQUIRE(j["print_info_total"]["mixed_filament_used"].get<bool>() == true);

    // Both plates sliced successfully.
    REQUIRE(j["plates"][0]["success"].get<bool>());
    REQUIRE(j["plates"][1]["success"].get<bool>());
    // Plate 1: virtual slots expanded to all 4 physical slots.
    REQUIRE(j["plates"][0]["filaments"].size() == 4);
    // Plate 2: pure physical — a single slot.
    REQUIRE(j["plates"][1]["filaments"].size() == 1);

    rm("/tmp/orca_capi_mix_twoplate.gcode.3mf");
    rm("/tmp/orca_capi_mix_twoplate.json");
    rm("/tmp/orca_capi_mix_twoplate.log");
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
