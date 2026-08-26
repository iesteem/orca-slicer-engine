#include "PresetRollback.hpp"

#include "Types.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <cmath>

namespace Slic3r
{

// ---- Read m_config ----

std::string PresetRollback::getFilamentType(const DynamicPrintConfig& config, int extruder_idx)
{
    if (!config.has("filament_type"))
        return {};

    auto* opt = config.option<ConfigOptionStrings>("filament_type");
    if (!opt || extruder_idx < 0 || extruder_idx >= static_cast<int>(opt->values.size()))
        return {};

    return opt->values[extruder_idx];
}

std::string PresetRollback::getFilamentVendor(const DynamicPrintConfig& config, int extruder_idx)
{
    if (!config.has("filament_vendor"))
        return {};

    auto* opt = config.option<ConfigOptionStrings>("filament_vendor");
    if (!opt || extruder_idx < 0 || extruder_idx >= static_cast<int>(opt->values.size()))
        return {};

    return opt->values[extruder_idx];
}

// ---- Find base category preset ----
//
// 3D exact-match strategy:
//   Pass 1 — same vendor: filament_vendor + filament_type + nozzle_diameter all match
//            (e.g. Snapmaker + PLA + 0.4 → "Snapmaker PLA Matte @U1 0.4 nozzle")
//   Pass 2 — Generic fallback: filament_vendor == "Generic" + filament_type + nozzle_diameter
//            (e.g. "Generic PLA @U1 0.4 nozzle")
//
// Nozzle match: via filament's compatible_printers → target printer preset → nozzle_diameter numeric comparison.

// Use the filament's compatible_printers to filter down to the target printer,
// read its nozzle_diameter, and compare against the target value (epsilon test).
static bool filament_supports_nozzle(const PresetBundle& bundle, const Preset& filament, double target_nozzle,
                                     const std::string& printer_model)
{
    auto* cp = filament.config.option<ConfigOptionStrings>("compatible_printers");
    if (!cp || cp->values.empty())
        return false;

    for (const std::string& printer_name : cp->values)
    {
        // Only consider printers matching the target model (e.g. "Snapmaker U1")
        if (printer_name.find(printer_model) == std::string::npos)
            continue;

        const Preset* printer = nullptr;
        for (const Preset& p : bundle.printers)
        {
            if (p.name == printer_name)
            {
                printer = &p;
                break;
            }
        }
        if (!printer)
            continue;

        auto* nd = printer->config.option<ConfigOptionFloats>("nozzle_diameter");
        if (!nd || nd->values.empty())
            continue;

        if (std::abs(nd->values[0] - target_nozzle) <= 0.001)
            return true;
    }
    return false;
}

// Read the preset's filament_type config value
static std::string get_preset_filament_type(const Preset& preset)
{
    auto* opt = preset.config.option<ConfigOptionStrings>("filament_type");
    if (!opt || opt->values.empty())
        return {};
    return opt->values[0];
}

// Read the preset's filament_vendor config value
static std::string get_preset_filament_vendor(const Preset& preset)
{
    auto* opt = preset.config.option<ConfigOptionStrings>("filament_vendor");
    if (!opt || opt->values.empty())
        return {};
    return opt->values[0];
}

// Find a system preset in bundle matching {vendor_name, filament_type, nozzle_diameter, printer_model}
static const Preset* find_in_vendor(const PresetBundle& bundle, const std::string& filament_type,
                                    const std::string& vendor_name, double nozzle_diameter,
                                    const std::string& printer_model)
{
    for (const Preset& preset : bundle.filaments)
    {
        if (!preset.is_system)
            continue;

        // Nozzle diameter match (compatible_printers → target printer → nozzle_diameter)
        if (!filament_supports_nozzle(bundle, preset, nozzle_diameter, printer_model))
            continue;

        // filament_type match (case-insensitive)
        if (!boost::iequals(get_preset_filament_type(preset), filament_type))
            continue;

        // filament_vendor match (case-insensitive)
        if (!boost::iequals(get_preset_filament_vendor(preset), vendor_name))
            continue;

        return &preset;
    }

    return nullptr;
}

const Preset* PresetRollback::findBaseFilament(const PresetBundle* bundle, const std::string& filament_type,
                                               const std::string& vendor_hint, double nozzle_diameter,
                                               const std::string& printer_model)
{
    if (!bundle || filament_type.empty())
        return nullptr;

    // Pass 1: same vendor
    if (!vendor_hint.empty())
    {
        const Preset* r = find_in_vendor(*bundle, filament_type, vendor_hint, nozzle_diameter, printer_model);
        if (r)
            return r;
    }

    // Pass 2: Generic fallback (avoid duplicate lookup when vendor_hint is already Generic)
    if (!boost::iequals(vendor_hint, "Generic"))
    {
        return find_in_vendor(*bundle, filament_type, "Generic", nozzle_diameter, printer_model);
    }

    return nullptr;
}

// ---- Shared full-overwrite operation ----
//
// Full overwrite: replace the extruder_idx slot in target with all per-extruder
// vector values from the source preset. Shared by both the inheritance-chain
// substitution and filament rollback paths to guarantee consistent semantics.

void PresetRollback::overwriteExtruderFrom(DynamicPrintConfig& target, const Preset& source, int extruder_idx,
                                           ConfigOptionStrings* filament_ids)
{
    if (extruder_idx < 0)
        return;
    const size_t dst_idx = static_cast<size_t>(extruder_idx);

    for (auto it = source.config.cbegin(); it != source.config.cend(); ++it)
    {
        const auto& key = it->first;
        const auto& src_opt = it->second;

        ConfigOption* dst_opt = target.option(key, true);
        if (!dst_opt)
            continue;

        auto* dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
        if (!dst_vec)
            continue;
        if (dst_vec->size() <= dst_idx)
            continue;

        auto* src_vec = dynamic_cast<const ConfigOptionVectorBase*>(src_opt.get());
        if (src_vec && src_vec->size() > 0)
            dst_vec->set_at(src_vec, dst_idx, 0);
    }

    // Update filament_settings_id to the source preset name
    if (filament_ids && dst_idx < filament_ids->values.size())
        filament_ids->values[dst_idx] = source.name;
}

// ---- Full rollback operation ----

bool PresetRollback::rollback(DynamicPrintConfig& config, const PresetBundle* bundle, int extruder_idx)
{
    // 1. Read current filament attributes
    const std::string filament_type = getFilamentType(config, extruder_idx);
    const std::string filament_vendor = getFilamentVendor(config, extruder_idx);

    if (filament_type.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "PresetRollback: filament_type not set for extruder " << extruder_idx
                                   << ", cannot rollback";
        return false;
    }

    // 1b. Read nozzle_diameter and printer_model (for compatible_printers → printer nozzle match)
    double nozzle_diameter = DEFAULT_NOZZLE_DIAMETER;
    if (config.has("nozzle_diameter"))
    {
        auto* nozzle_opt = config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt && extruder_idx >= 0 && static_cast<size_t>(extruder_idx) < nozzle_opt->values.size())
            nozzle_diameter = nozzle_opt->values[extruder_idx];
    }

    std::string printer_model = DEFAULT_PRINTER_MODEL;

    // 2. Find base category preset
    const Preset* base = findBaseFilament(bundle, filament_type, filament_vendor, nozzle_diameter, printer_model);
    if (!base)
    {
        BOOST_LOG_TRIVIAL(warning) << "PresetRollback: no base filament found for type=\"" << filament_type
                                   << "\" vendor=\"" << filament_vendor << "\" nozzle=" << nozzle_diameter;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "PresetRollback: rolling back extruder " << extruder_idx << " from type=\""
                            << filament_type << "\" to base preset \"" << base->name << "\"";

    // 3. Full overwrite + update filament_settings_id (shared helper)
    auto* fsi =
        config.has("filament_settings_id") ? config.option<ConfigOptionStrings>("filament_settings_id", true) : nullptr;
    overwriteExtruderFrom(config, *base, extruder_idx, fsi);

    return true;
}

} // namespace Slic3r
