#include "PresetRollback.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <cmath>

namespace Slic3r {

// ---- 读取 m_config ----

std::string PresetRollback::getFilamentType(const DynamicPrintConfig& config, int extruder_idx)
{
    if (!config.has("filament_type")) return {};

    auto* opt = config.option<ConfigOptionStrings>("filament_type");
    if (!opt || extruder_idx < 0 || extruder_idx >= static_cast<int>(opt->values.size()))
        return {};

    return opt->values[extruder_idx];
}

std::string PresetRollback::getFilamentVendor(const DynamicPrintConfig& config, int extruder_idx)
{
    if (!config.has("filament_vendor")) return {};

    auto* opt = config.option<ConfigOptionStrings>("filament_vendor");
    if (!opt || extruder_idx < 0 || extruder_idx >= static_cast<int>(opt->values.size()))
        return {};

    return opt->values[extruder_idx];
}

// ---- 查找基础大类预设 ----
//
// 三维精确匹配策略：
//   Pass 1 — 同厂商：filament_vendor + filament_type + nozzle_diameter 全匹配
//            （如 Snapmaker + PLA + 0.4 → "Snapmaker PLA Matte @U1 0.4 nozzle"）
//   Pass 2 — Generic 兜底：filament_vendor == "Generic" + filament_type + nozzle_diameter
//            （如 "Generic PLA @U1 0.4 nozzle"）
//
// 喷嘴匹配：通过 filament 的 compatible_printers → 对应 printer 预设 → nozzle_diameter 数值比较。

// 通过 filament 的 compatible_printers，筛出目标机型的 printer，
// 读取 printer 的 nozzle_diameter 与目标值做 ε 比较。
static bool filament_supports_nozzle(const PresetBundle& bundle,
                                      const Preset& filament,
                                      double target_nozzle,
                                      const std::string& printer_model)
{
    auto* cp = filament.config.option<ConfigOptionStrings>("compatible_printers");
    if (!cp || cp->values.empty()) return false;

    for (const std::string& printer_name : cp->values) {
        // 只考虑目标机型的 printer（如 "Snapmaker U1"）
        if (printer_name.find(printer_model) == std::string::npos) continue;

        const Preset* printer = nullptr;
        for (const Preset& p : bundle.printers) {
            if (p.name == printer_name) {
                printer = &p;
                break;
            }
        }
        if (!printer) continue;

        auto* nd = printer->config.option<ConfigOptionFloats>("nozzle_diameter");
        if (!nd || nd->values.empty()) continue;

        if (std::abs(nd->values[0] - target_nozzle) <= 0.001)
            return true;
    }
    return false;
}

// 读取 preset 的 filament_type 配置值
static std::string get_preset_filament_type(const Preset& preset)
{
    auto* opt = preset.config.option<ConfigOptionStrings>("filament_type");
    if (!opt || opt->values.empty()) return {};
    return opt->values[0];
}

// 读取 preset 的 filament_vendor 配置值
static std::string get_preset_filament_vendor(const Preset& preset)
{
    auto* opt = preset.config.option<ConfigOptionStrings>("filament_vendor");
    if (!opt || opt->values.empty()) return {};
    return opt->values[0];
}

// 在 bundle 中找 {vendor_name, filament_type, nozzle_diameter, printer_model} 全匹配的系统预设
static const Preset* find_in_vendor(const PresetBundle& bundle,
                                     const std::string& filament_type,
                                     const std::string& vendor_name,
                                     double nozzle_diameter,
                                     const std::string& printer_model)
{
    for (const Preset& preset : bundle.filaments) {
        if (!preset.is_system) continue;

        // 喷嘴直径匹配（compatible_printers → 目标机型 printer → nozzle_diameter）
        if (!filament_supports_nozzle(bundle, preset, nozzle_diameter, printer_model)) continue;

        // filament_type 匹配（大小写不敏感）
        if (!boost::iequals(get_preset_filament_type(preset), filament_type)) continue;

        // filament_vendor 匹配（大小写不敏感）
        if (!boost::iequals(get_preset_filament_vendor(preset), vendor_name)) continue;

        return &preset;
    }

    return nullptr;
}

const Preset* PresetRollback::findBaseFilament(const PresetBundle* bundle,
                                                const std::string& filament_type,
                                                const std::string& vendor_hint,
                                                double nozzle_diameter,
                                                const std::string& printer_model)
{
    if (!bundle || filament_type.empty()) return nullptr;

    // Pass 1: 同厂商
    if (!vendor_hint.empty()) {
        const Preset* r = find_in_vendor(*bundle, filament_type, vendor_hint, nozzle_diameter, printer_model);
        if (r) return r;
    }

    // Pass 2: Generic 兜底（避免 vendor_hint 已经是 Generic 时重复查）
    if (!boost::iequals(vendor_hint, "Generic")) {
        return find_in_vendor(*bundle, filament_type, "Generic", nozzle_diameter, printer_model);
    }

    return nullptr;
}

// ---- 共享「全面覆盖」操作 ----
//
// Full overwrite: 用 source 预设的所有 per-extruder 向量值，覆盖 target 中
// extruder_idx 槽位。与「继承链替换」「耗材回退」两条路径共用，确保语义一致。

void PresetRollback::overwriteExtruderFrom(DynamicPrintConfig& target,
                                           const Preset& source,
                                           int extruder_idx,
                                           ConfigOptionStrings* filament_ids)
{
    if (extruder_idx < 0) return;
    const size_t dst_idx = static_cast<size_t>(extruder_idx);

    for (auto it = source.config.cbegin(); it != source.config.cend(); ++it) {
        const auto& key     = it->first;
        const auto& src_opt = it->second;

        ConfigOption* dst_opt = target.option(key, true);
        if (!dst_opt) continue;

        auto* dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
        if (!dst_vec) continue;
        if (dst_vec->size() <= dst_idx) continue;

        auto* src_vec = dynamic_cast<const ConfigOptionVectorBase*>(src_opt.get());
        if (src_vec && src_vec->size() > 0)
            dst_vec->set_at(src_vec, dst_idx, 0);
    }

    // 更新 filament_settings_id 为 source 名称
    if (filament_ids && dst_idx < filament_ids->values.size())
        filament_ids->values[dst_idx] = source.name;
}

// ---- 完整回退操作 ----

bool PresetRollback::rollback(DynamicPrintConfig& config,
                               const PresetBundle* bundle,
                               int extruder_idx)
{
    // 1. 读取当前耗材属性
    const std::string filament_type   = getFilamentType(config, extruder_idx);
    const std::string filament_vendor = getFilamentVendor(config, extruder_idx);

    if (filament_type.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "PresetRollback: filament_type not set for extruder "
            << extruder_idx << ", cannot rollback";
        return false;
    }

    // 1b. 读取 nozzle_diameter 和 printer_model（用于 compatible_printers → printer 喷嘴匹配）
    double nozzle_diameter = 0.4;
    if (config.has("nozzle_diameter")) {
        auto* nozzle_opt = config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt && extruder_idx >= 0
            && static_cast<size_t>(extruder_idx) < nozzle_opt->values.size())
            nozzle_diameter = nozzle_opt->values[extruder_idx];
    }

    std::string printer_model = "Snapmaker U1";

    // 2. 查找基础大类预设
    const Preset* base = findBaseFilament(bundle, filament_type, filament_vendor,
                                          nozzle_diameter, printer_model);
    if (!base) {
        BOOST_LOG_TRIVIAL(warning)
            << "PresetRollback: no base filament found for type=\""
            << filament_type << "\" vendor=\"" << filament_vendor
            << "\" nozzle=" << nozzle_diameter;
        return false;
    }

    BOOST_LOG_TRIVIAL(info)
        << "PresetRollback: rolling back extruder " << extruder_idx
        << " from type=\"" << filament_type << "\" to base preset \""
        << base->name << "\"";

    // 3. Full overwrite + 更新 filament_settings_id（共享 helper）
    auto* fsi = config.has("filament_settings_id")
                  ? config.option<ConfigOptionStrings>("filament_settings_id", true)
                  : nullptr;
    overwriteExtruderFrom(config, *base, extruder_idx, fsi);

    return true;
}

} // namespace Slic3r
