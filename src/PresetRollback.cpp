#include "PresetRollback.h"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

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
// 两级回退策略：
//   Pass 1 — 同厂商基础类：vendor 匹配且 filament_type 匹配（如 Snapmaker PLA → "Snapmaker PLA @U1 base"）
//   Pass 2 — 通用基础类：OrcaFilamentLibrary / Generic 中 filament_type 匹配（如 "Generic PLA @System"）

static const Preset* find_in_vendor(const PresetBundle& bundle,
                                     const std::string& filament_type,
                                     const std::string& vendor_name)
{
    const Preset* best      = nullptr;
    int           best_score = -1;

    for (const Preset& preset : bundle.filaments) {
        if (!preset.is_system && !preset.is_default) continue;
        if (!preset.vendor) continue;
        if (!boost::iequals(preset.vendor->name, vendor_name)) continue;

        auto* ft_opt = preset.config.option<ConfigOptionStrings>("filament_type");
        if (!ft_opt || ft_opt->values.empty()) continue;
        if (!boost::iequals(ft_opt->values[0], filament_type)) continue;

        int score = 0;

        // 名称含 "base" → 是中间基础层，优先
        if (preset.name.find("base") != std::string::npos ||
            preset.name.find("Base") != std::string::npos)
            score += 100;

        // instantiation=false → 中间层模板，优先
        auto* inst_opt = preset.config.option<ConfigOptionBool>("instantiation");
        if (inst_opt && !inst_opt->value)
            score += 80;

        // inherits 指向 fdm_filament_* → 接近根大类
        const std::string& inherits_to = preset.inherits();
        if (inherits_to.find("fdm_filament_") != std::string::npos)
            score += 60;
        else if (!inherits_to.empty())
            score += 30;

        // 不含 nozzle 后缀 → 更像基础类
        if (preset.name.find("nozzle") == std::string::npos
            && preset.name.find("0.2") == std::string::npos
            && preset.name.find("0.4") == std::string::npos)
            score += 40;

        if (score > best_score) {
            best_score = score;
            best       = &preset;
        }
    }

    return best;
}

static const Preset* find_generic(const PresetBundle& bundle,
                                   const std::string& filament_type)
{
    const Preset* best      = nullptr;
    int           best_score = -1;

    for (const Preset& preset : bundle.filaments) {
        if (!preset.is_system && !preset.is_default) continue;

        auto* ft_opt = preset.config.option<ConfigOptionStrings>("filament_type");
        if (!ft_opt || ft_opt->values.empty()) continue;
        if (!boost::iequals(ft_opt->values[0], filament_type)) continue;

        int score = 0;

        // OrcaFilamentLibrary → 通用跨厂商基础类
        if (preset.m_from_orca_filament_lib)
            score += 100;

        // 名称含 "Generic" → 明确的通用预设
        if (preset.name.find("Generic") != std::string::npos)
            score += 80;

        // instantiation=false → 中间层
        auto* inst_opt = preset.config.option<ConfigOptionBool>("instantiation");
        if (inst_opt && !inst_opt->value)
            score += 60;

        // 无 inherits → 根大类
        const std::string& inherits_to = preset.inherits();
        if (inherits_to.empty())
            score += 50;
        else if (inherits_to.find("fdm_filament_common") != std::string::npos)
            score += 40;

        if (score > best_score) {
            best_score = score;
            best       = &preset;
        }
    }

    return best;
}

const Preset* PresetRollback::findBaseFilament(const PresetBundle* bundle,
                                                const std::string& filament_type,
                                                const std::string& vendor_hint)
{
    if (!bundle || filament_type.empty()) return nullptr;

    // Pass 1: 同厂商基础类
    if (!vendor_hint.empty()) {
        const Preset* vendor_base = find_in_vendor(*bundle, filament_type, vendor_hint);
        if (vendor_base) return vendor_base;
    }

    // Pass 2: 通用基础类 (Generic / OrcaFilamentLibrary)
    return find_generic(*bundle, filament_type);
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

    // 2. 查找基础大类预设
    const Preset* base = findBaseFilament(bundle, filament_type, filament_vendor);
    if (!base) {
        BOOST_LOG_TRIVIAL(warning)
            << "PresetRollback: no base filament found for type=\""
            << filament_type << "\" vendor=\"" << filament_vendor << "\"";
        return false;
    }

    BOOST_LOG_TRIVIAL(info)
        << "PresetRollback: rolling back extruder " << extruder_idx
        << " from type=\"" << filament_type << "\" to base preset \""
        << base->name << "\"";

    // 3. Full overwrite: all per-extruder values from the base-category preset.
    //    Same strategy as substitute_filament_params() — no user values preserved.
    const size_t dst_idx = static_cast<size_t>(extruder_idx);
    for (auto it = base->config.cbegin(); it != base->config.cend(); ++it) {
        const auto& key     = it->first;
        const auto& src_opt = it->second;

        ConfigOption* dst_opt = config.option(key, true);
        if (!dst_opt) continue;

        auto* dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
        if (!dst_vec) continue;
        if (dst_vec->size() <= dst_idx) continue;

        auto* src_vec = dynamic_cast<const ConfigOptionVectorBase*>(src_opt.get());
        if (src_vec && src_vec->size() > 0)
            dst_vec->set_at(src_vec, dst_idx, 0);
    }

    // 4. 更新 filament_settings_id 为基础大类名称
    if (config.has("filament_settings_id")) {
        auto* fsi = config.option<ConfigOptionStrings>("filament_settings_id", true);
        if (fsi && extruder_idx < static_cast<int>(fsi->values.size())) {
            fsi->values[extruder_idx] = base->name;
        }
    }

    return true;
}

} // namespace Slic3r
