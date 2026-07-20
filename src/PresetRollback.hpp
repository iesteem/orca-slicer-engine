#pragma once

#include <string>

namespace Slic3r
{

class DynamicPrintConfig;
class Preset;
class PresetBundle;
class ConfigOptionStrings;

// ============================================================================
// PresetRollback — Roll back user-defined filament presets to matching library presets
//
// Use case:
//   In cloud enforce mode, if slicing fails after official preset substitution,
//   rollback() can revert the target extruder's filament to a library preset
//   matching {filament_vendor, filament_type, nozzle_diameter}.
//
// Data flow:
//   m_config (3MF project_settings.config) ──── filament_type, filament_vendor, nozzle_diameter
//   PresetBundle::filaments (system preset library) ──── 3D match lookup
//   m_config ← per-extruder key parameters from the matched library preset
// ============================================================================
class PresetRollback
{
public:
    PresetRollback() = default;
    ~PresetRollback() = default;

    // ---- Read filament attributes from m_config ----

    /// Read filament_type for the specified extruder (e.g. "PLA", "PETG", "ABS")
    static std::string getFilamentType(const DynamicPrintConfig& config, int extruder_idx);

    /// Read filament_vendor for the specified extruder (e.g. "Generic", "Snapmaker")
    static std::string getFilamentVendor(const DynamicPrintConfig& config, int extruder_idx);

    // ---- Find base category preset ----

    /// Find a base category preset in PresetBundle::filaments matching filament_type.
    ///
    /// 4D exact-match strategy:
    ///   Pass 1 — same vendor: filament_vendor + filament_type + nozzle_diameter + printer_model all match
    ///            (e.g. Snapmaker + PLA + 0.4 + Snapmaker U1 → "Snapmaker PLA @U1 0.4 nozzle")
    ///   Pass 2 — Generic fallback: filament_vendor == "Generic" + nozzle_diameter + printer_model
    ///            (e.g. "Generic PLA @U1 0.4 nozzle")
    ///
    /// Returns nullptr if no matching base category preset is found.
    static const Preset* findBaseFilament(const PresetBundle* bundle, const std::string& filament_type,
                                          const std::string& vendor_hint = {}, double nozzle_diameter = 0.4,
                                          const std::string& printer_model = "Snapmaker U1");

    // ---- Shared full-overwrite operation ----

    /// Replace all per-extruder vector parameters at the extruder_idx slot in
    /// target with values from the source preset (no user values are preserved),
    /// and update filament_ids at that index to source.name.
    ///
    /// Shared by both the inheritance-chain substitution and filament rollback
    /// paths to guarantee consistent replacement semantics.
    /// filament_ids may be nullptr (parameters only, no name update).
    static void overwriteExtruderFrom(DynamicPrintConfig& target, const Preset& source, int extruder_idx,
                                      ConfigOptionStrings* filament_ids);

    // ---- Full rollback operation ----

    /// Replace the specified extruder's filament with a matching library preset.
    ///
    /// Steps:
    ///   1. Read filament_type, filament_vendor, nozzle_diameter from config
    ///   2. 3D match lookup in bundle for a library preset
    ///   3. Write matched preset's per-extruder parameters to the target extruder slot in config
    ///   4. Update filament_settings_id to the matched preset name
    ///
    /// Returns: true = rollback succeeded, false = no matching library preset found.
    static bool rollback(DynamicPrintConfig& config, const PresetBundle* bundle, int extruder_idx);
};

} // namespace Slic3r
