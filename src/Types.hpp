#pragma once

// Umbrella / forwarding header — the historical single include for engine
// sources and tests. Definitions live in focused headers:
//   ExitCodes.hpp   — EXIT_* aliases of the C ABI SLIC3R_* codes
//   Issue.hpp       — IssueLevel / Issue / make_* factories / GLOBAL_ISSUE
//   SliceStats.hpp  — SliceOutputStats (FilamentDetail / PlateStats)
//   TempFileGuard.hpp — RAII temp-file cleanup guard
// Existing #include "Types.hpp" lines keep working unchanged; new code may
// include the focused header directly.

#include "ExitCodes.hpp"
#include "Issue.hpp"
#include "SliceStats.hpp"
#include "TempFileGuard.hpp"

// Preset gate constant — the ONLY printer model this engine slices. Consumed
// by the Stage 1.1 gate (SliceEngine::validate_printer_model) and, as a
// provably-redundant fallback, by PresetRollback (see the invariant note
// there). Do NOT extend into a generic default for other printer families.
constexpr const char* DEFAULT_PRINTER_MODEL = "Snapmaker U1";

// Output format enum
enum class OutputFormat
{
    GCODE,
    GCODE_3MF
};
