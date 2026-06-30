#include "PresetRollback.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <cctype>

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

// 判断一个系统预设是否为「官方」耗材：Snapmaker 厂商，或来自
// OrcaFilamentLibrary 跨厂商通用库。与 SliceEngine::validate_filament_official
// 中的 is_official_preset 判据保持一致——回退目标必须同样是官方耗材，
// 否则会把非官方耗材「回退」到另一个非官方 base，静默绕过强制官方不变量。
static bool is_official_filament(const Preset& p)
{
    if (p.vendor && boost::iequals(p.vendor->name, PresetBundle::SM_BUNDLE))
        return true;
    if (p.m_from_orca_filament_lib)
        return true;
    return false;
}

// 判断预设名是否带「具体喷嘴」后缀（如 "... @U1 0.6 nozzle"、"... @0.2 nozzle"）。
// 基础大类预设不含喷嘴标记（如 "... @base"、"... @U1 base"）。
//
// 用独立 token "nozzle" 作为判据，而非匹配直径数字子串——后者会误中
// "0.25"（被 find("0.2") 命中）等命名，且需硬编码直径列表。OrcaSlicer
// 资源中所有带喷嘴的 filament 预设均以 " <直径> nozzle" 结尾，故
// "nozzle" 词的存在即充分信号；filament 预设本身不携带 nozzle_diameter 字段。
static bool name_has_nozzle_suffix(const std::string& name)
{
    const std::string token = "nozzle";
    size_t pos = name.find(token);
    while (pos != std::string::npos) {
        // 要求 "nozzle" 是独立单词：前后非字母数字（或位于串首/尾）
        bool left_ok  = (pos == 0) || !std::isalnum(static_cast<unsigned char>(name[pos - 1]));
        size_t after  = pos + token.size();
        bool right_ok = (after >= name.size()) || !std::isalnum(static_cast<unsigned char>(name[after]));
        if (left_ok && right_ok)
            return true;
        pos = name.find(token, pos + 1);
    }
    return false;
}

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
        // 回退目标必须是官方耗材，否则会把非官方耗材回退到另一个
        // 非官方 base，静默绕过强制官方不变量（R2）。
        if (!is_official_filament(preset)) continue;

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

        // 不含具体喷嘴后缀 → 更像基础类
        if (!name_has_nozzle_suffix(preset.name))
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
        // 通用回退同样只接受官方耗材（OrcaFilamentLibrary 或 Snapmaker），
        // 防止选中非官方厂商自带的 "Generic <type>"（R2）。
        if (!is_official_filament(preset)) continue;

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

    // 3. Full overwrite + 更新 filament_settings_id（共享 helper）
    auto* fsi = config.has("filament_settings_id")
                  ? config.option<ConfigOptionStrings>("filament_settings_id", true)
                  : nullptr;
    overwriteExtruderFrom(config, *base, extruder_idx, fsi);

    return true;
}

} // namespace Slic3r
