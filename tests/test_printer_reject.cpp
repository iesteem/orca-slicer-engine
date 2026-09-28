/**
 * @file test_printer_reject.cpp
 * @brief Integration test for validate_printer_model rejection of non-U1
 * printers.
 *
 * Drives the full run() on projects authored for other printers (Bambu P1S,
 * Creality K1, Snapmaker J1) and a U1 project whose printer_model was hand-
 * edited to a fake value. All must be rejected at validate_printer_model with
 * EXIT_PREPROCESS_ERROR (6) and a PRINTER_MODEL_UNSUPPORTED error whose message
 * names the offending model.
 * Tag: [integration][printer].
 */

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Utils.hpp"

#include "Types.hpp"                   // IssueLevel, EXIT_PREPROCESS_ERROR
#include "SliceEngine.hpp"

static const char* kResources = ORCA_TEST_RESOURCES;

namespace {

// Run the full pipeline; return engine for inspection.
std::unique_ptr<SliceEngine> run_on(const std::string& input_file)
{
    Slic3r::set_resources_dir(kResources);
    Slic3r::set_data_dir(kResources);

    EngineConfig cfg;
    cfg.input_file = input_file;
    cfg.skip_preset_substitution = false;
    cfg.max_size_mb = 0;
    cfg.temp_dir = "/tmp";

    auto engine = std::make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()));
    engine->run();
    return engine;
}

// Find the PRINTER_MODEL_UNSUPPORTED error issue; returns its message (empty if absent).
std::string find_unsupported_msg(const SliceOutputStats& s)
{
    for (const auto& iss : s.issues)
        if (iss.code == "PRINTER_MODEL_UNSUPPORTED" && iss.level == IssueLevel::error)
            return iss.message;
    return {};
}
} // namespace

// Common rejection contract: exit 6, error, no success, PRINTER_MODEL_UNSUPPORTED
// fired, and the message names the offending model.
void check_rejected_with_model(const std::string& fixture, const std::string& expected_model)
{
    auto e = run_on(std::string(ORCA_TEST_FIXTURE_DIR) + "/printer_reject/" + fixture);
    REQUIRE(e->exit_code() == EXIT_PREPROCESS_ERROR);
    REQUIRE(e->any_error());
    REQUIRE_FALSE(e->stats().success);

    const std::string msg = find_unsupported_msg(e->stats());
    REQUIRE_FALSE(msg.empty());
    INFO("issue message: " << msg << "; expected model: " << expected_model);
    REQUIRE(msg.find(expected_model) != std::string::npos);
}

TEST_CASE("Bambu P1S rejected as unsupported printer", "[integration][printer]")
{
    check_rejected_with_model("bambu_p1s.3mf", "Bambu Lab P1S");
}

TEST_CASE("Creality K1 rejected as unsupported printer", "[integration][printer]")
{
    check_rejected_with_model("creality_k1.3mf", "Creality K1");
}

TEST_CASE("Snapmaker J1 rejected as unsupported printer", "[integration][printer]")
{
    // J1 is a Snapmaker printer but not the U1 — still rejected.
    check_rejected_with_model("snapmaker_j1.3mf", "Snapmaker J1");
}

TEST_CASE("Hand-edited fake printer model rejected, no crash", "[integration][printer]")
{
    // Fixture name: "直接修改打印机型号为banbu U1，切片不崩溃，切片失败". The model
    // string is a fake "BanBu U1" (not the real Snapmaker U1). Verifies the
    // rejection path does not crash on an adversarial hand-edited value.
    check_rejected_with_model("modified_to_bambu_u1.3mf", "BanBu U1");
}
