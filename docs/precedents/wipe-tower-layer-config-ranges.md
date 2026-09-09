# P-03: 高度范围分层换色模型擦除塔缺失

**状态：** 已修复（上游 OrcaSlicer `5cac51bbe5`，分支 feature-2.3.5-base，已 push）
**Fixture：** filement_count/layer_range_multicolor（tests/test_filament_count.cpp）

## 现象

"高度范围分层换色"模型（颜色按 z 区间分配，`Metadata/layer_config_ranges.xml`）
擦除塔不生成、克重不含换色 purge，与桌面版差异大。

## 根因链（在上游 libslic3r，不在引擎）

1. `layer_config_ranges.xml` 被 bbs_3mf.cpp:2787 **正确解析**（日志里的
   "skipped ... not supported" 是另一 zip 条目打的，误导）
2. 高度换色的用色存在 `wall_filament`/`sparse_infill_filament`/
   `solid_infill_filament`，`extruder` 键是默认 0
3. `Print::object_extruders()`（Print.cpp:888-898）遍历 layer_config_ranges
   **只读 `extruder` 键**（value>0 才收）→ 漏读三个 per-feature 键
4. `normalize_fdm_2`（PrintApply.cpp:1300）时 PrintRegion 未建立 →
   `used_filaments=1` → **强制 `enable_prime_tower=false`**，塔关了不再开

## 修复

`Print::object_extruders()` 的 layer_config_ranges 循环补充收集三个
per-feature 键（+14 行）。引擎经静态库自动生效。

## 已证伪（勿重走）

❌ "删 trim 修掉了高度范围换色误判单色"——该验证只看 stats 报双色，
**没检查擦除塔**。删 trim 后塔仍缺失（本 bug 与 trim 无关）。

## 验证铁律

**擦除塔是否真生成必须看 gcode 里 `WIPE_TOWER_START` 次数 + gcode 头
`; enable_prime_tower`**，不能只看 stats 报双色。`enable_prime_tower` 在
project_settings.config 是 1 但 gcode 头是 0 = normalize_fdm_2 关塔的证据。
