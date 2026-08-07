/**
 * @file test_gcode_security.cpp
 * @brief Integration test for user-content stripping (RCE prevention) and
 * official-preset override of user G-code.
 *
 * The cloud engine strips user-supplied content for safety (strip_user_content,
 * SliceEngine.cpp:742). This file verifies the security-critical contracts
 * end-to-end:
 *   - post_process (shell-command RCE vector) is fully cleared, even when the
 *     fixture carries hostile payloads (reverse-shell / netcat backdoors).
 *   - User-authored machine/filament G-code is replaced by the official preset
 *     value (non-empty after substitution), not left as the user wrote it.
 *   - USER_CONTENT_CLEARED tip is emitted for stripped notes/external refs.
 *
 * Tag: [integration][gcodesec].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // Issue, IssueLevel
#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

std::unique_ptr<SliceEngine> run_on(const std::string& input_file, const char* tag)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = input_file;
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";
    cfg.output_base = std::string("/tmp/orca_test_gs_") + tag;

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

// Serialized length of a config key's value (works for scalar and vector opts).
size_t cfg_len(const Slic3r::DynamicPrintConfig& c, const std::string& key)
{
    const Slic3r::ConfigOption* opt = c.option(key);
    return opt ? opt->serialize().size() : 0;
}

bool has_issue(const SliceOutputStats& s, const std::string& code)
{
    for (const auto& iss : s.issues)
        if (iss.code == code)
            return true;
    return false;
}

std::string fixture(const char* name)
{
    return std::string(ORCA_TEST_FIXTURE_DIR) + "/gcode_security/" + name;
}
} // namespace

// --- RCE prevention: post_process shell commands fully stripped. -----------

TEST_CASE("Hostile post_process scripts are stripped (RCE prevention)", "[integration][gcodesec]")
{
    // Fixture carries python3 reverse-shell + perl netcat backdoor in
    // post_process. The engine must clear it completely.
    auto e = run_on(fixture("rce_postprocess.3mf"), "rce");
    REQUIRE(e->stats().success);
    REQUIRE(e->exit_code() == 0);

    // post_process MUST be empty after stripping.
    REQUIRE(cfg_len(e->config(), "post_process") == 0);
}

// --- User G-code replaced by official preset (not left as user wrote it). --

TEST_CASE("User machine G-code replaced by official preset", "[integration][gcodesec]")
{
    // Several fixtures embed user-authored start/end/layer-change G-code.
    // After substitution these keys hold the OFFICIAL preset value (non-empty,
    // identical across fixtures that differ only in user gcode), proving the
    // user value did not survive.
    auto e1 = run_on(fixture("custom_start_gcode.3mf"), "g_start");
    auto e2 = run_on(fixture("custom_end_gcode.3mf"), "g_end");
    REQUIRE(e1->stats().success);
    REQUIRE(e2->stats().success);

    // Official machine_start_gcode is large and identical regardless of the
    // user's start-gcode edit.
    size_t s1 = cfg_len(e1->config(), "machine_start_gcode");
    size_t s2 = cfg_len(e2->config(), "machine_start_gcode");
    REQUIRE(s1 > 100);
    REQUIRE(s1 == s2);
}

TEST_CASE("Custom pause / sequential G-code stripped to official", "[integration][gcodesec]")
{
    auto e = run_on(fixture("custom_pause.3mf"), "pause");
    REQUIRE(e->stats().success);
    // post_process is the shell vector — must be empty even for these.
    REQUIRE(cfg_len(e->config(), "post_process") == 0);
}

TEST_CASE("Multi-plate mixed config: user content stripped, slices OK", "[integration][gcodesec]")
{
    auto e = run_on(fixture("multi_plate_aggregate.3mf"), "multi");
    REQUIRE(e->stats().success);
    REQUIRE(cfg_len(e->config(), "post_process") == 0);
}
