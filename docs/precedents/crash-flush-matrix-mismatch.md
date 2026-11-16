# P-02: flush_volumes_matrix 维度不匹配 → ToolOrdering SIGSEGV

**状态：** 已修复（commit fe5c61c，移植 PresetBundle 规整逻辑）
**Fixture：** `~/Downloads/【Lucas造物】永生小盆栽系列-迷你蘑菇.3mf`（外部）

## 现象

`Generating skirt & brim`（70%）阶段 SIGSEGV，栈：
`ToolOrdering::tools_for_layer → Print::process → run_slicing`。

## 根因

模型自带**脏数据**：`filament_diameter` 4 槽但 `flush_volumes_matrix` 是 3×3
（作者改了 AMS 配色没同步 flush matrix）。崩溃链：

1. `has_wipe_tower()==true` → `Print.cpp:3137 _make_wipe_tower`
2. `:3145 number_of_extruders = sqrt(9) = 3`
3. `:3154 安全检查 filament_diameter.size()(4) > 3` 命中 → **直接 return**，
   既没构造 `m_wipe_tower_data.tool_ordering` 也没构造成员 `m_tool_ordering`
4. psSkirtBrim 阶段无条件取空 `m_tool_ordering` → 空解引用

**桌面版为何不崩：** GUI 加载 3MF 后调 `PresetBundle::update_multi_material_
filament_presets`（PresetBundle.cpp:3625）把 flush matrix 规整到 N×N
（:3650-3675）。该规整只在 GUI 代码被调用，无头引擎从不触发。

## 修复

引擎在 `normalize_loaded_config()`（run() 加载后全局执行一次，早于逐板
处理）中调 `normalize_flush_volumes_matrix(DynamicPrintConfig&)`（移植
PresetBundle.cpp:3650-3675）：按 `filament_diameter.size()` 重建 matrix 为
N×N、`flush_volumes_vector` 为 2N。维度已匹配时 early-return（正常模型零
改动）。（历史上该调用位于 `prepare_merged_config_for_plate`，后上移至
`normalize_loaded_config()`。）

验证：迷你蘑菇从 SIGSEGV@70% → 完整切出 4.8MB gcode.3mf，4 色全解析。

## 遗留独立问题

引擎把该模型读成 4 槽，但模型实际配 2 个 filament_settings、桌面版产物 2 色。
本修复只对齐维度消崩溃，未触碰槽位计数正确性。

## 关联

同族"配置数据不一致致数据结构损坏"：P-01。
