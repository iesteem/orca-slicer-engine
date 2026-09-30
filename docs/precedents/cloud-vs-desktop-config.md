# P-15: 云引擎 vs 桌面端配置流程对比（参考架构）

**状态：** 2026-06-29 分析定稿；这是理解本引擎一切配置行为差异的基础地图

## 3MF 内部结构（Orca 导出）

| 内部文件 | 内容 |
|---|---|
| `Metadata/project_settings.config` | full_fff_config() 全量快照 + `different_settings_to_system`、`inherits_group` |
| `Metadata/process_settings_N.config` | 嵌入预设相对 inherits 父的差异值 |
| `Metadata/{filament,machine}_settings_N.config` | 同上 |
| `Metadata/layer_config_ranges.xml` | 高度范围换色（bbs_3mf.cpp 解析，见 P-03） |

## 核心差异表

| 维度 | 云引擎 | 桌面端 |
|---|---|---|
| different_settings_to_system | **完全不存在** | 核心机制：仅"不同"的 key 覆盖系统预设 |
| Printer/Filament 预设 | 无条件全量官方替换 | load_external_preset + different_settings 过滤 |
| G-code | 强制替换为官方（防 RCE，P 见 test_gcode_security） | 弹窗用户确认后保留 |
| 云端特有 | PresetRollback 回退、sanitize_config、禁 post_process | 用户可写预设 |

## 实际影响（哪些设置与桌面版一致）

- Process（层高/速度/填充/支撑）：一致 ✅
- Printer/Filament/G-code：**不一致（有意设计）**——云强制官方值 ❌
- Project 级（wipe tower/color/bed type）：一致 ✅

## 嵌入预设判例（mini train city）

`find_official_process_preset` 的 `find_in_system` 必须过滤
`is_project_embedded`（空壳内嵌预设曾被当官方套用 → negative spacing →
SLICING_EXCEPTION）。2026-08-18 起产品定义：工艺预设替换必须成功，找不到
官方祖先 → EXIT_PREPROCESS_ERROR(6) + PROCESS_PRESET_NOT_OFFIAL。
3MF zip 内改 JSON 必须用 `zout.writestr(item, data)` 保留原 ZipInfo，否则
中文文件名 UTF-8 flag 丢失 → LOAD_ERROR(3)。

## 3MF 双导入器坑

OrcaSlicer 有两套导入器：`Format/3mf.cpp`（Prusa，读
`Metadata/Prusa_Slicer_layer_config_ranges.xml`）和 `Format/bbs_3mf.cpp`
（Bambu，读 `Metadata/layer_config_ranges.xml`）。引擎走
`Model::read_from_file` → bbs 路径。**查 3MF 格式务必看 bbs_3mf.cpp。**

## 其他已验证事实

- `Preset::is_compatible` 引擎从不计算（无 update_compatible 调用），恒
  true——PresetRollback 查找不需要也不应加该过滤。
- `inherits_group` 配置解析已端到端验证（v02.01.03，11 盘多色全对）。
