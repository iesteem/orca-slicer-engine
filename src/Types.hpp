#pragma once

/**
 * @file Types.hpp
 * @brief Convenience umbrella header for all engine type definitions.
 *
 * Includes the four sub-headers that compose the type system:
 *   - ExitCodes.hpp   — process exit codes
 *   - Issue.hpp       — Issue struct, OutputFormat enum, factory functions
 *   - SliceStats.hpp  — SliceOutputStats and nested per-plate stats
 *   - TempFileGuard.hpp — RAII temp-file cleanup guard
 *
 * New code should include only the specific header(s) it needs rather
 * than this umbrella.  This header exists for backward compatibility.
 */

#include "ExitCodes.hpp"
#include "Issue.hpp"
#include "SliceStats.hpp"
#include "TempFileGuard.hpp"

// Cloud slicer engine version (independent of SLIC3R_VERSION)
// When compiling the standalone consumer (CMake defines CLOUD_SLICER_ENGINE_VERSION
// as a macro for signal-safe string concatenation), the constexpr is skipped to
// avoid collision.
#ifndef CLOUD_SLICER_ENGINE_VERSION
constexpr const char* CLOUD_SLICER_ENGINE_VERSION = "02.01.18";
#endif
