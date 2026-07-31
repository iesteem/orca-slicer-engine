#pragma once

#include <string>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;
class PresetBundle;

// ============================================================================
// PresetRollback — rolls a user-customised filament preset back to a base
// category preset (e.g. Generic PLA/PETG).
//
// Use case:
//   In the cloud engine's enforce mode, if slicing fails after the official
//   preset substitution, rollback() can fall the target extruder's filament
//   back to a base category preset in OrcaFilamentLibrary whose filament_type
//   matches.
//
// Data flow:
//   m_config (3MF project_settings.config) ──── filament_type, filament_vendor
//   PresetBundle::filaments (system preset library) ──── find a matching base category
//   m_config ← per-extruder key parameters from the base category preset
// ============================================================================
class PresetRollback
{
public:
    PresetRollback()  = default;
    ~PresetRollback() = default;

    // ---- Read filament attributes from m_config ----

    /// Read the filament_type of the given extruder (e.g. "PLA", "PETG", "ABS").
    static std::string getFilamentType(const DynamicPrintConfig& config, int extruder_idx);

    /// Read the filament_vendor of the given extruder (e.g. "Generic", "Snapmaker").
    static std::string getFilamentVendor(const DynamicPrintConfig& config, int extruder_idx);

    // ---- Find a base category preset ----

    /// Find a base category preset in PresetBundle::filaments whose filament_type matches.
    ///
    /// Two-tier fallback strategy:
    ///   Pass 1 — same-vendor base: vendor match + filament_type match
    ///            (e.g. "Snapmaker" + "PLA" -> "Snapmaker PLA @U1 base")
    ///   Pass 2 — generic base: filament_type match within OrcaFilamentLibrary / Generic
    ///            (e.g. "Generic PLA @System")
    ///
    /// Returns nullptr if no matching base category preset is found.
    static const Preset* findBaseFilament(const PresetBundle* bundle,
                                          const std::string& filament_type,
                                          const std::string& vendor_hint = {});

    // ---- Full rollback operation ----

    /// Replace the given extruder's filament with a base category preset.
    ///
    /// Steps:
    ///   1. Read filament_type and filament_vendor from config
    ///   2. Find a base category preset in bundle
    ///   3. Write the base preset's key parameters into config at the extruder's slot
    ///   4. Update filament_settings_id to the base category name
    ///
    /// Return value: true = rollback succeeded, false = no matching base preset found.
    static bool rollback(DynamicPrintConfig& config,
                         const PresetBundle* bundle,
                         int extruder_idx);
};

} // namespace Slic3r
