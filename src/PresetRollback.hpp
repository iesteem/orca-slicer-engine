#pragma once

#include <string>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;
class PresetBundle;
class ConfigOptionStrings;

// ============================================================================
// PresetRollback — 将用户自定义耗材预设回退到匹配的基础库预设
//
// 使用场景：
//   云引擎 enforce 模式下，官方预设替换后若切片失败，可通过
//   rollback() 将目标 extruder 的耗材回退到库中匹配
//   {filament_vendor, filament_type, nozzle_diameter} 的预设。
//
// 数据流：
//   m_config (3MF project_settings.config) ──── filament_type, filament_vendor, nozzle_diameter
//   PresetBundle::filaments (系统预设库) ──── 三维匹配查找
//   m_config ← 匹配的库预设的 per-extruder 关键参数
// ============================================================================
class PresetRollback
{
public:
    PresetRollback()  = default;
    ~PresetRollback() = default;

    // ---- 读取 m_config 中的耗材属性 ----

    /// 读取指定 extruder 的 filament_type（如 "PLA", "PETG", "ABS"）
    static std::string getFilamentType(const DynamicPrintConfig& config, int extruder_idx);

    /// 读取指定 extruder 的 filament_vendor（如 "Generic", "Snapmaker"）
    static std::string getFilamentVendor(const DynamicPrintConfig& config, int extruder_idx);

    // ---- 查找基础大类预设 ----

    /// 在 PresetBundle::filaments 中查找匹配 filament_type 的基础大类预设。
    ///
    /// 四维精确匹配策略：
    ///   Pass 1 — 同厂商：filament_vendor + filament_type + nozzle_diameter + printer_model 全匹配
    ///            （如 Snapmaker + PLA + 0.4 + Snapmaker U1 → "Snapmaker PLA @U1 0.4 nozzle"）
    ///   Pass 2 — Generic 兜底：filament_vendor == "Generic" + nozzle_diameter + printer_model
    ///            （如 "Generic PLA @U1 0.4 nozzle"）
    ///
    /// 返回 nullptr 表示未找到任何匹配的基础大类预设。
    static const Preset* findBaseFilament(const PresetBundle* bundle,
                                          const std::string& filament_type,
                                          const std::string& vendor_hint = {},
                                          double nozzle_diameter = 0.4,
                                          const std::string& printer_model = "Snapmaker U1");

    // ---- 共享「全面覆盖」操作 ----

    /// 用 source 预设的全部 per-extruder 向量参数，全面覆盖 target 中
    /// extruder_idx 对应槽位的值（不保留任何用户值），并把 filament_ids
    /// 在该索引处更新为 source.name。
    ///
    /// 「继承链替换」与「耗材回退」两条路径共用此 helper，确保替换语义一致。
    /// filament_ids 可为 nullptr（仅覆盖参数、不更新名称）。
    static void overwriteExtruderFrom(DynamicPrintConfig& target,
                                      const Preset& source,
                                      int extruder_idx,
                                      ConfigOptionStrings* filament_ids);

    // ---- 完整回退操作 ----

    /// 将指定 extruder 的耗材替换为匹配的库预设。
    ///
    /// 操作步骤：
    ///   1. 从 config 读取 filament_type、filament_vendor、nozzle_diameter
    ///   2. 在 bundle 中三维匹配查找库预设
    ///   3. 将匹配预设的 per-extruder 参数写入 config 对应的 extruder 位置
    ///   4. 更新 filament_settings_id 为匹配预设名称
    ///
    /// 返回值：true = 回退成功，false = 未找到匹配的库预设。
    static bool rollback(DynamicPrintConfig& config,
                         const PresetBundle* bundle,
                         int extruder_idx);
};

} // namespace Slic3r
