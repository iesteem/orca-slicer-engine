#pragma once

#include <string>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;
class PresetBundle;

// ============================================================================
// PresetRollback — 将用户自定义耗材预设回退到基础大类（如 Generic PLA/PETG）
//
// 使用场景：
//   云引擎 enforce 模式下，官方预设替换后若切片失败，可通过
//   rollback() 将目标 extruder 的耗材回退到 OrcaFilamentLibrary
//   中匹配 filament_type 的基础大类预设。
//
// 数据流：
//   m_config (3MF project_settings.config) ──── filament_type, filament_vendor
//   PresetBundle::filaments (系统预设库) ──── 查找匹配的基础大类
//   m_config ← 基础大类预设的 per-extruder 关键参数
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
    /// 两级回退策略：
    ///   Pass 1 — 同厂商基础类：vendor 匹配 + filament_type 匹配
    ///            （如 "Snapmaker" + "PLA" → "Snapmaker PLA @U1 base"）
    ///   Pass 2 — 通用基础类：OrcaFilamentLibrary / Generic 中 filament_type 匹配
    ///            （如 "Generic PLA @System"）
    ///
    /// 返回 nullptr 表示未找到任何匹配的基础大类预设。
    static const Preset* findBaseFilament(const PresetBundle* bundle,
                                          const std::string& filament_type,
                                          const std::string& vendor_hint = {});

    // ---- 完整回退操作 ----

    /// 将指定 extruder 的耗材替换为基础大类预设。
    ///
    /// 操作步骤：
    ///   1. 从 config 读取 filament_type 和 filament_vendor
    ///   2. 在 bundle 中查找基础大类预设
    ///   3. 将基础大类预设的关键参数写入 config 对应的 extruder 位置
    ///   4. 更新 filament_settings_id 为基础大类名称
    ///
    /// 返回值：true = 回退成功，false = 未找到匹配的基础大类预设。
    static bool rollback(DynamicPrintConfig& config,
                         const PresetBundle* bundle,
                         int extruder_idx);
};

} // namespace Slic3r
