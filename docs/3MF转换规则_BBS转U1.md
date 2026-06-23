# 3MF 转换规则：Bambu/Prusa → Snapmaker U1

> 对应脚本：`tools/convert_3mf_from_bbs_to_u1.py`

## 概述

将 Bambu Lab / Prusa 切片软件生成的 3MF 文件转换为 Snapmaker U1 兼容格式。仅修改 `project_settings.config` 和 `slice_info.config`，不添加 machine/process/filament settings -- 这些从引擎内置的 U1 配置文件继承。

---

## 1. 移除 Bambu/Prusa 专用 Keys

从 `project_settings.config` 中删除约 50 个 Bambu/Prusa 专用配置项，包括：

| 类别 | Keys |
|------|------|
| 打印机通信 | `host_type`, `print_host`, `printhost_apikey`, `printhost_authorization_type`, `printhost_port`, `printhost_ssl`, `printhost_user` |
| G-code 风格 | `gcode_flavor`, `inherits`, `inherits_group`, `printer_notes`, `printer_structure`, `printer_variant` |
| 特殊功能 | `scan_first_layer`, `silent_mode`, `remaining_times`, `timelapse_type`, `thumbnail_size`, `thumbnails_custom_color` |
| 腔室控制 | `chamber_temperatures`, `chamber_temperature_comment`, `support_chamber_temp_control` |
| 挤出机间隔 | `extruder_clearance_dist_to_rod`, `extruder_clearance_max_radius`, `extruder_clearance_height_to_lid`, `extruder_clearance_height_to_rod` |
| 其他 Bambu 专用 | `extruder_type`, `filament_scarf_*`, `initial_layer_flow_ratio`, `ironing_direction`, `overhang_threshold_participating_cooling`, `overhang_totally_speed`, `physical_printer_settings_id`, `role_base_wipe_speed`, `smooth_coefficient`, `smooth_speed_discontinuity_area`, `sparse_infill_anchor*`, `top_area_threshold`, `top_one_wall_type`, `template_custom_gcode`, `use_relative_e_distances`, `wiping_volumes_matrix` |

---

## 2. 打印机身份替换

| Key | 转换规则 |
|-----|---------|
| `printer_model` | → `"Snapmaker U1"` |
| `printer_settings_id` | 从 `nozzle_diameter` 提取喷嘴直径 → `"Snapmaker U1 (0.4 nozzle)"` |
| `print_settings_id` | 追加 `" (converted)"` 后缀 |
| `version` | → `"2.3.1"` |

---

## 3. 热床尺寸

| Key | 值 |
|-----|---|
| `printable_area` | `["0.5x1", "270.5x1", "270.5x271", "0.5x271"]` |
| `printable_height` | `"270.05"` |
| `bed_exclude_area` | `["0x0"]` |

---

## 4. 喷嘴设置

| Key | 转换规则 |
|-----|---------|
| `nozzle_type` | → `"hardened_steel"` |
| `nozzle_volume` | → `"143"` |
| `nozzle_diameter` | 每个元素夹持到 U1 支持的尺寸 `[0.2, 0.4, 0.6, 0.8]`（取最接近值），数组长度扩展至与耗材数量匹配 |

---

## 5. 速度和加速度上限

将以下值裁剪至 U1 安全上限。百分比值（含 `%`）跳过不处理。值为 `0` 时视为"使用默认值"，替换为上限值。

### 速度上限 (mm/s)

| Key | 上限 |
|-----|-----|
| `travel_speed` | 500 |
| `outer_wall_speed` | 200 |
| `inner_wall_speed` | 300 |
| `sparse_infill_speed` | 270 |
| `top_surface_speed` | 200 |
| `gap_infill_speed` | 250 |
| `internal_solid_infill_speed` | 250 |
| `bridge_speed` | 50 |
| `support_speed` | 150 |
| `support_interface_speed` | 80 |
| `initial_layer_speed` | 50 |
| `initial_layer_infill_speed` | 105 |
| `skirt_speed` | 50 |
| `wipe_speed` | 75 |
| `ironing_speed` | 30 |

### 加速度上限 (mm/s²)

| Key | 上限 |
|-----|-----|
| `default_acceleration` | 10000 |
| `outer_wall_acceleration` | 5000 |
| `inner_wall_acceleration` | 10000 |
| `initial_layer_acceleration` | 500 |
| `top_surface_acceleration` | 2000 |
| `travel_acceleration` | 10000 |

---

## 6. 支撑耗材索引

| Key | 转换规则 |
|-----|---------|
| `support_filament` | 0-based → 1-based（`"0"` → `"1"`） |
| `support_interface_filament` | 0-based → 1-based（`"0"` → `"1"`） |

---

## 7. G-code 替换

检测并替换所有 Bambu 专用 G-code 模板。检测关键字：`bbl`, `bambu`, `m1007`, `g392`, `m620`, `m622`, `m9833`, `m73 p`, `a1`, `p1s`, `x1c`, `m400 u1`, `m991`。

6 个 G-code 模板全部替换为 Snapmaker U1 等效版本：

| 模板 | 说明 |
|------|------|
| `machine_start_gcode` | 包含 `PRINT_START`, `DEFECT_DETECTION_*`, `BED_MESH_CALIBRATE` 等 |
| `machine_end_gcode` | `PRINT_END` + `TIMELAPSE_STOP` |
| `before_layer_change_gcode` | `TIMELAPSE_TAKE_FRAME` + `DEFECT_DETECTION_DETECT` |
| `layer_change_gcode` | `SET_PRINT_STATS_INFO` 含 `{total_layer_count}` / `{layer_num}` |
| `change_filament_gcode` | `T[next_extruder]` + `M109` |
| `machine_pause_gcode` | `M600` |

`time_lapse_gcode` 清空（U1 原生支持延时摄影）。

---

## 8. 冲洗/擦料塔参数

### 单耗材打印

| Key | 转换规则 |
|-----|---------|
| `flush_volumes_matrix` | → `["0"]` |
| `enable_prime_tower` | → `"0"` |
| `prime_volume` | → `"0"` |
| `flush_into_infill` | → `"0"` |
| `flush_into_objects` | → `"0"` |

### 多耗材打印

保留原值（归零会导致错误代码 `rc=6`）。

### 通用

| Key | 转换规则 |
|-----|---------|
| `flush_volumes_vector` | → `["140"] × n_filaments` |
| `wiping_volumes_extruders` | → `["70"] × 10` |

---

## 9. 耗材名称重映射

将 Bambu/Prusa 耗材名称映射为 Snapmaker U1 官方预设名称，确保 `filament_settings_id` 能被引擎在系统预设中匹配（快速路径 #1）。

### 映射流程

1. **去除 Bambu 标注**：移除 `(xxx.3mf)` 文件名标注、`@BBL X1C` / `@Prusa MK4` 等打印机后缀
2. **检测材料类型**：从名称中识别 `PLA`, `PETG`, `PET`, `ABS`, `ASA`, `TPU`, `TPE`, `PA`, `PC`, `PVA`, `NYLON`, `SUPPORT`
3. **检测子类型**：识别 `basic`, `matte`, `silk`, `eco`, `wood`, `metal`, `marble`, `glow`, `snapspeed`, `high speed`, `cf`, `carbon fiber`, `full spectrum`, `95a`, `90a`, `hf`, `high flow`
4. **查表映射**：`(材料, 子类型)` → Snapmaker U1 预设名称
5. **关键词回退**：未匹配的类型使用材料关键词回退

### 完整映射表

| 源材料 | 子类型 | 目标预设 |
|--------|--------|---------|
| PLA | basic | `Snapmaker PLA Basic @U1` |
| PLA | matte | `Snapmaker PLA Matte @U1` |
| PLA | silk | `Snapmaker PLA Silk @U1` |
| PLA | eco | `Snapmaker PLA Eco @U1` |
| PLA | wood | `Snapmaker PLA Wood @U1` |
| PLA | metal | `Snapmaker PLA Metal @U1` |
| PLA | marble | `Snapmaker PLA Marble @U1` |
| PLA | glow | `Snapmaker PLA Glow @U1` |
| PLA | snapspeed / high speed | `Snapmaker PLA SnapSpeed @U1` |
| PLA | cf / carbon fiber | `Snapmaker PLA-CF @U1` |
| PLA | full spectrum | `Snapmaker PLA Full Spectrum @U1` |
| PLA | (none) | `Snapmaker PLA @U1` |
| PETG | cf / carbon fiber | `Snapmaker PETG-CF @U1` |
| PETG | (none) | `Snapmaker PETG @U1` |
| PET | (none) | `Snapmaker PET @U1` |
| ABS | (none) | `Snapmaker ABS @U1` |
| ASA | (none) | `Snapmaker ASA @U1` |
| TPU | 95a | `Snapmaker TPU 95A @U1` |
| TPU | 90a | `Snapmaker TPU 90A @U1` |
| TPU | hf / high flow | `Snapmaker TPU High-Flow @U1` |
| TPU | (none) | `Snapmaker TPU @U1` |
| TPE | (none) | `Snapmaker TPE @U1` |
| PA | cf / carbon fiber | `Snapmaker PA-CF @U1` |
| PA / NYLON | (none) | `Generic PA @System` |
| PC | (none) | `Generic PC @System` |
| PVA | (none) | `Snapmaker PVA @U1` |
| SUPPORT | pla / (none) | `Snapmaker Breakaway Support For PLA @U1` |

### 名称重映射覆盖的 Keys

`filament_type`, `filament_settings_id`, `default_filament_colour`, `filament_colour`, `support_filament`, `support_interface_filament`

---

## 10. U1 默认 Key 注入

添加约 200+ 个 Snapmaker U1 默认配置项，涵盖：

- **腔室与温度控制**：`activate_chamber_temp_control`, `chamber_temperature`, `graphic_effect_plate_temp`, `textured_cool_plate_temp` 等
- **耗材装载/卸载**：`filament_loading_speed`, `filament_unloading_speed`, `filament_toolchange_delay` 等
- **冷却与风扇**：`fan_kickstart`, `fan_speedup_overhangs`, `fan_speedup_time`, `internal_bridge_fan_speed` 等
- **Ramming 参数**：`filament_ramming_parameters`, `filament_multitool_ramming` 等
- **Prime Tower**：`wipe_tower_*` 系列
- **模糊皮肤**：`fuzzy_skin_mode`, `fuzzy_skin_point_distance`, `fuzzy_skin_thickness` 等
- **填充与实体**：`infill_anchor`, `internal_bridge_*`, `bottom_solid_infill_flow_ratio`, `top_surface_density` 等
- **接缝与坡度**：`seam_slope_type`, `seam_slope_min_length`, `staggered_inner_seams` 等
- **缩水补偿**：`filament_shrinkage_compensation_z`
- **延时摄影**：`thumbnails`, `thumbnails_format`, `bbl_calib_mark_logo`
- **振动补偿**：`resonance_avoidance`, `max_resonance_avoidance_speed`, `min_resonance_avoidance_speed`
- **挤出机与回退**：`cooling_tube_retraction`, `cooling_tube_length`, `parking_pos_retraction` 等
- **Z 偏移与裙边**：`z_offset`, `skirt_type`, `skirt_start_angle`, `min_skirt_length`
- **Scarf 接缝**：`scarf_joint_flow_ratio`, `scarf_joint_speed`, `scarf_overhang_threshold`
- **支撑**：`support_ironing`, `support_threshold_overlap`, `tree_support_*` 等
- **其他**：`precise_outer_wall`, `only_one_wall_top`, `hole_to_polyhole`, `interlocking_*` 等

> 完整列表见脚本中的 `KEYS_TO_ADD` 字典。

---

## 11. slice_info.config 更新

- 将 `<metadata key="printer_model_id">` 的值改为 `"Snapmaker U1"`
- 移除所有 `<warning>` 元素（可能引用 Bambu 专用信息）

---

## 12. Filament Settings 文件处理

### 已有文件

- 重映射其中的 `filament_settings_id`、`filament_type`、`filament_vendor` 为 U1 名称
- 修复 `inherits`：重映射名称，为空时回填 `filament_settings_id`
- 如果耗材名称映射到了官方 Snapmaker 预设，**删除该文件**（系统预设处理，避免循环继承）
- 将 `from` 强制设为 `"project"`，`version` 设为 `"02.03.01.00"`

### 缺失文件

如果源文件中没有 `filament_settings_N.config` 文件，则为每个非官方 Snapmaker 预设的挤出机生成一个。每个文件包含：
- 从 `project_settings` 提取的逐挤出机参数（温度、回退、压力提前等）
- `inherits` 链设置
- 跳过已是官方系统预设的挤出机

---

## 13. 元数据清理

| 操作 | 说明 |
|------|------|
| 移除 `Metadata/cut_information.xml` | Bambu 专用断料信息 |
| 添加 `Metadata/plate_1.json` | 如果缺失，添加默认热床包围盒 `[0, 0, 270, 270]` |
| 跳过 Bambu 专用元数据 | `host_type`, `gcode_flavor`, `printhost_*` 等 |

---

## 14. from 字段策略

**不强制修改** `from` 字段为 `"project"`。

当 `from="project"` 时引擎会在 3MF 内部的 filament_settings 文件中查找预设，但已跳过了那些映射到官方 Snapmaker 名称的文件（避免循环继承）。保留原始值让引擎能在系统预设库中正确查找。

---

## 15. 验证检查项

转换后执行 7 项验证：

| # | 检查项 | 级别 |
|---|--------|------|
| 1 | `printer_model` 是否为 `"Snapmaker U1"` | CRITICAL |
| 2 | `nozzle_type` 是否为 `"hardened_steel"` | WARNING |
| 3 | `printable_area` / `printable_height` 是否匹配 U1 尺寸 | CRITICAL |
| 4 | `filament_settings_id` 是否为空或非官方名称 | WARNING |
| 5 | filament_settings 文件的 `inherits` 是否为空 | CRITICAL |
| 6 | 是否存在残留的 Bambu 专用 keys | WARNING |
| 7 | `printer_settings_id` 是否包含 `"Snapmaker U1"` | WARNING |
