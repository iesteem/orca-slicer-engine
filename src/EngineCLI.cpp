/**
 * @file EngineCLI.cpp
 * @brief Implementation of CLI parsing and bootstrap helpers.
 */

#include "EngineCLI.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/support/date_time.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

#include "ExitCodes.hpp"
#include "Utils.hpp"

namespace EngineCLI {

// ============================================================================
// parse_args
// ============================================================================

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_OK;
            std::exit(EXIT_OK);
        }
        else if (arg == "--dump-config-schema") {
            std::cout << dump_config_schema(Slic3r::print_config_def);
            std::exit(EXIT_OK);
        }
        else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        }
        else if (arg == "--log") {
            args.log_enabled = true;
        }
        else if (arg == "--log-file" && i + 1 < argc) {
            args.log_enabled   = true;
            args.log_file_path = argv[++i];
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            args.engine_cfg.output_base = argv[++i];
        }
        else if ((arg == "-r" || arg == "--resources") && i + 1 < argc) {
            args.resources_dir = argv[++i];
        }
        else if (arg == "-j" || arg == "--json") {
            args.json_enabled = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                args.json_output_path = argv[++i];
        }
        else if ((arg == "-p" || arg == "--plate") && i + 1 < argc) {
            std::string plate_arg = argv[++i];
            if (plate_arg == "all" || plate_arg == "0") {
                args.engine_cfg.plate_id = 0;
            } else {
                try {
                    args.engine_cfg.plate_id = std::stoi(plate_arg);
                    if (args.engine_cfg.plate_id < 1) {
                        std::cerr << "Error: Plate ID must be 1 or greater (or 'all')." << std::endl;
                        BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                        std::exit(EXIT_INVALID_ARGS);
                    }
                } catch (...) {
                    std::cerr << "Error: Invalid plate ID: " << plate_arg << std::endl;
                    BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                    std::exit(EXIT_INVALID_ARGS);
                }
            }
        }
        else if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
            try {
                args.engine_cfg.timeout_seconds = std::stoi(argv[++i]);
                if (args.engine_cfg.timeout_seconds < 0)
                    args.engine_cfg.timeout_seconds = 0;
            } catch (...) {
                std::cerr << "Error: Invalid timeout value." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else if ((arg == "--max-size") && i + 1 < argc) {
            try {
                int val = std::stoi(argv[++i]);
                args.engine_cfg.max_size_mb = (val < 0) ? 0 : val;
            } catch (...) {
                std::cerr << "Error: Invalid max-size value." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else if (arg == "--cancel-file" && i + 1 < argc) {
            args.engine_cfg.cancel_file = argv[++i];
        }
        else if (arg == "--allow-custom-presets") {
            args.engine_cfg.substitute_printer  = false;
            args.engine_cfg.substitute_filaments = false;
        }
        else if (arg == "--allow-custom-printer-presets") {
            args.engine_cfg.substitute_printer = false;
        }
        else if (arg == "--allow-custom-filament-presets") {
            args.engine_cfg.substitute_filaments = false;
        }
        else if (arg == "--threads" && i + 1 < argc) {
            try {
                int val = std::stoi(argv[++i]);
                args.engine_cfg.thread_count = (val < 0) ? 0 : val;
            } catch (...) {
                std::cerr << "Error: Invalid thread count." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "gcode") {
                args.engine_cfg.format = OutputFormat::GCODE;
            } else if (fmt == "gcode.3mf") {
                args.engine_cfg.format = OutputFormat::GCODE_3MF;
            } else {
                std::cerr << "Error: Unknown format: " << fmt
                          << " (use 'gcode' or 'gcode.3mf')" << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        // Only parameters that are not recognized as options/option values are considered input files
        else if (arg[0] != '-') {
            if (args.engine_cfg.input_file.empty()) {
                args.engine_cfg.input_file = arg;
            } else {
                std::cerr << "Error: Multiple input files specified." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else {
            std::cerr << "Error: Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
            std::exit(EXIT_INVALID_ARGS);
        }
    }

    // Validate that an input file was provided
    if (args.engine_cfg.input_file.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        print_usage(argv[0]);
        BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
        std::exit(EXIT_INVALID_ARGS);
    }

    return args;
}

// ============================================================================
// detect_resources_dir
// ============================================================================

std::string detect_resources_dir() {
    namespace fs = boost::filesystem;

    fs::path exe_dir = boost::dll::program_location().parent_path();

    // Prefer ../resources/ (Ubuntu packaging: bin/orca-slice-engine -> ../resources/)
    fs::path parent_resources = exe_dir.parent_path() / "resources";
    if (fs::exists(parent_resources))
        return parent_resources.string();

    // Fall back to ./resources/ (development layout)
    fs::path local_resources = exe_dir / "resources";
    if (fs::exists(local_resources))
        return local_resources.string();

    // Last resort: environment variable
    const char* env = std::getenv("ORCA_RESOURCES");
    if (env)
        return std::string(env);

    return {};
}

// ============================================================================
// setup_logging
// ============================================================================

void setup_logging(bool log_enabled, const std::string& log_file_path, bool verbose) {
    if (log_enabled) {
        namespace expr = boost::log::expressions;

        // Ensure the parent directory exists
        boost::filesystem::path log_parent =
            boost::filesystem::path(log_file_path).parent_path();
        if (!log_parent.empty() && !boost::filesystem::exists(log_parent))
            boost::filesystem::create_directories(log_parent);

        boost::log::add_file_log(
            boost::log::keywords::file_name = log_file_path,
            boost::log::keywords::auto_flush = true,
            boost::log::keywords::format = (
                expr::stream
                    << "[" << expr::format_date_time<boost::posix_time::ptime>(
                        "TimeStamp", "%Y-%m-%d %H:%M:%S")
                    << "] [" << boost::log::trivial::severity
                    << "] " << expr::smessage
            )
        );
    }

    if (verbose)
        boost::log::core::get()->set_filter(
            boost::log::trivial::severity >= boost::log::trivial::trace);
}

// ============================================================================
// compute_expected_output_path
// ============================================================================

std::string compute_expected_output_path(const EngineConfig& cfg) {
    return generate_output_path(
        cfg.input_file, cfg.output_base, cfg.plate_id, cfg.format, cfg.single_plate);
}

} // namespace EngineCLI
