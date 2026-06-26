#pragma once

#include <string>

#include "SliceEngine.hpp"

/**
 * @file EngineCLI.hpp
 * @brief Shared CLI argument parsing and bootstrap logic for the cloud slicing engine.
 *
 * Extracted from main.cpp so that the same parsing, resource detection, and
 * logging setup can be reused across the standalone executable, the C API
 * wrapper, and future consumers.
 */

/**
 * @brief Parsed CLI arguments that drive the engine session.
 *
 * Most fields map 1:1 to EngineConfig members; the extras (json/log paths,
 * verbose flag) are CLI-only concerns not needed by SliceEngine itself.
 */
struct CliArgs {
    EngineConfig engine_cfg;       //!< Engine configuration derived from CLI flags.
    std::string  resources_dir;    //!< Path to OrcaSlicer resources directory.
    std::string  json_output_path; //!< Explicit --json output path (empty = auto).
    bool         json_enabled = false; //!< Whether JSON statistics output is requested.
    bool         log_enabled  = false; //!< Whether file logging is enabled.
    std::string  log_file_path;    //!< Explicit --log-file path (empty = auto).
    bool         verbose      = false; //!< Enable verbose/trace logging.
};

/**
 * @brief Free functions for CLI parsing, resource detection, logging setup,
 *        and output path pre-computation.
 *
 * These are pure functions (no mutable global state) so they can be called
 * from any entry point (main, C API, test harness).
 */
namespace EngineCLI {

/**
 * @brief Parse command-line arguments into a CliArgs struct.
 *
 * Exits the process with EXIT_INVALID_ARGS on parse errors or
 * EXIT_OK when --help or --dump-config-schema is used.
 *
 * @param argc Argument count (as received by main).
 * @param argv Argument vector (as received by main).
 * @return Populated CliArgs (only returns on success).
 */
CliArgs parse_args(int argc, char* argv[]);

/**
 * @brief Auto-detect the OrcaSlicer resources directory.
 *
 * Search order:
 *   1. ../resources/   (Ubuntu packaging: bin/orca-slice-engine -> ../resources/)
 *   2. ./resources/    (development layout: build dir / resources/)
 *   3. $ORCA_RESOURCES environment variable
 *
 * @return Detected path, or empty string if none found.
 */
std::string detect_resources_dir();

/**
 * @brief Configure Boost.Log file output and verbosity.
 *
 * @param log_enabled   Whether file logging is enabled.
 * @param log_file_path Path to the log file (should be non-empty when
 *                      log_enabled is true; caller is expected to derive
 *                      it via compute_expected_output_path if needed).
 * @param verbose       When true, lower the severity threshold to trace.
 */
void setup_logging(bool log_enabled, const std::string& log_file_path, bool verbose);

/**
 * @brief Compute the expected output path for a given engine config.
 *
 * Delegates to generate_output_path() from Utils.
 * The result can be used to derive log-file and JSON-output paths
 * before any slicing actually runs.
 *
 * @param cfg  The engine configuration after parsing.
 * @return Pre-computed output path string.
 */
std::string compute_expected_output_path(const EngineConfig& cfg);

} // namespace EngineCLI
