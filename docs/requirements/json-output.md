# 需求：JSON 输出契约

> 来源实现：`src/JsonReport.cpp`（`build_statistics_json`）。本契约是引擎
> 对外最不可漂移的接口——下游解析直接依赖，任何字段变更都是 breaking
> change，需人类批准并记 CHANGELOG。
> **[anchor] 级契约**：整体重构时必须保持逐字段兼容。

## 输出位置

- stdout：包裹在 `=== SLICE STATISTICS (JSON) ===` / `=== END STATISTICS ===` 之间
- 文件：`-j`/`--json` 指定路径（写入失败仅 warning，不影响退出码）
- 缩进 2 空格，UTF-8

## 编号约定

- **`plate_id` 一律 1-based**（内部 0-based + 1；日志里 `Starting slicing
  for plate M` 是 0-based，勿混淆）
- **`filaments[].id` 一律 1-based**（内部 extruder 0-based + 1）

## 数值精度

所有数值字段经 `round2`（四舍五入 2 位小数；非有限值 → 0.0）。
`print_time_formatted` 为 `HHhMMm` / `MMmSSs` 风格（format_time_hhmmss）。

## 顶层结构

```jsonc
{
  "success": true,                    // bool；全部请求的盘成功才为 true
  "engine_version": "2.2.0",          // = ENGINE_VERSION（CMake 唯一来源）
  "resources_version": "02.03.03.03", // 可选；<resources>/profiles/Snapmaker.json 的
                                      // "version" 原样回显（不走 Semver，保留补零）。
                                      // 报告时读取，成功与失败出口均在场；
                                      // 缺席 = 版本未知，绝非特定版本
  "print_info_total": { ... },        // 跨成功盘聚合
  "issues": [ ... ],                  // 全局级 issue 数组
  "plates": [ ... ]                   // 逐盘
}
```

## print_info_total（聚合仅含 success=true 的盘）

```jsonc
{
  "output_file": "path",              // 失败时为 ""；成功时为输出路径
  "print_time_seconds": 1234.56,
  "print_time_formatted": "20h34m",
  "total_weight_g": 12.34,
  "total_length_mm": 12345.67,        // mm（内部按 m 累计 ×1000）
  "plate_count": 3,                   // stats.plates.size()，含失败盘
  "filaments": [                      // 跨盘按 (type, color) 去重，克重累加
    { "type": "PLA", "color": "#FFFFFF", "used_g": 5.92, "used_mm": 1966.7 }
  ],
  "support_free": true,               // 可选；仅整体 success=true 时输出——失败盘
                                      // 的支撑状态未判定，聚合即猜测。gcode 实际层：
                                      // true=所有盘 gcode 均零支撑挤出（配置开了但
                                      // 模型自支撑也归 true）；false=任一盘实际挤出
                                      // 了支撑结构（含筏，普通/树支撑均计）。缺席 =
                                      // 未知，绝非 true（缺席坍缩在保守侧）
  "mixed_filament_used": false        // 可选；plates[] 非空即输出——载入时一次判定
                                      // 的输入文件属性（对象/part extruder 或面片
                                      // 引用虚拟混色槽），部分失败运行仍有效。
                                      // 缺席（载入阶段失败，模型未解析）= 未知
}
```

## issue 对象（全局 issues 与 plate.issues 同构）

```jsonc
{
  "level": "error|serious_warning|warning|tip",
  "plate_id": 1,                      // 1-based；全局级为 -1（保持原值，不 +1）
  "object_name": "Body1",             // 可为空串
  "z_height": 27.0,
  "layer": 42,                        // 仅 >=0 时输出（可选字段）
  "code": "TOOLPATH_OUTSIDE",         // 见 exit-codes.md 的 code 表
  "message": "human readable",
  "suggestion": "..."                 // 仅非空时输出（可选字段）
}
```

## plates[] 元素

```jsonc
{
  "plate_id": 1,                      // 1-based
  "success": true,
  "issues": [ ... ],                  // 仅非空时输出该键（可选）
  // ---- 以下字段仅 success=true 时输出 ----
  "print_time_seconds": 1234.56,
  "print_time_formatted": "20h34m",
  "total_weight_g": 5.92,
  "filaments": [
    { "id": 1, "type": "PLA", "color": "#FFFFFF", "used_g": 5.92, "used_mm": 1966.7 }
  ],
  "model_thumbnail": "<base64 PNG>",  // 可为空串
  "long_retraction_when_cut": false,
  "support_free": true                // gcode 实际层：该盘支撑挤出体积为 0（含配置
                                      // 开启但自支撑的场景；普通/树/筏支撑均计）
}
```

## 历史变更记录（防漂移）

| 版本 | 变更 |
|---|---|
| 2.2.0 (2026-10-10) | 顶层新增可选 `resources_version`（vendor profile 版本原样回显，缺席=未知）；`print_info_total` 新增可选 `support_free`（gcode 实际层：true=零支撑挤出，仅整体 success 时输出）与 `mixed_filament_used`（plates[] 非空即输出，输入文件属性）；`plates[]` success 组新增 `support_free`（同 gcode 实际层语义）。三者缺席一律=未知，非 false/空串；support_free 缺席坍缩在保守侧（"有支撑"）。同版 issue 码新增见 exit-codes.md（NO_PLATES_PRODUCED、PLATE_MEMBERSHIP_REBUILT、INSTANCE_NOT_ON_ANY_PLATE） |
| v02.01.07 (2026-09-24) | 无 plate 元数据 3MF 合成单盘：`issues[]` 新增 tip 级 `PLATE_DATA_SYNTHESIZED`；此类文件 plates[] 首次可含成功盘（plate_id=1、无 model_thumbnail） |
| v02.01.10 (2026-06-11) | `plates[].filaments[].filament_id` → `id`。libslic3r 内部 `FilamentInfo::filament_id` 不变 |

## 已知语义注意点

- 失败盘的 `print_time/weight/filaments` 字段**不输出**（非零垃圾值不可
  断言，见 test_slice_e2e 的写测试点）——下游必须按"键缺失 = 无数据"
  处理，而非默认 0。
- `plate_count` 计**所有**盘（含失败），`print_info_total` 聚合**仅成功盘**
  ——两者口径不同。
- all-or-nothing 打包（P-05）下，任一盘失败 → `success=false` 且
  `output_file=""`，即使其余盘切片成功（其 per-plate 数据仍在 plates[] 里）。
- 无 plate 元数据的 3MF（第三方导出）按桌面 Orca 语义合成单盘切全部
  printable 实例，并发 tip 级 `PLATE_DATA_SYNTHESIZED`（云端可用其区分
  "真单盘文件"与"无盘文件被合成"）；合成盘无缩略图源，
  `model_thumbnail` 为空。
- `support_free` / `mixed_filament_used` / `resources_version` 均为
  **可选字段，缺席 = 未知**（各自输出条件见上文结构定义），下游禁止按
  "缺省即 false / 空串"读取。`support_free` 取反极性（2026-10-10 由
  `support_required` 改名翻转，未发布前原地修订）：有利状态（true=无需
  支撑）只能由显式 true 到达，缺席/误读缺省全部坍缩到保守侧"有支撑"。
- `support_free` 判定源是 **gcode 实际层**（同日二次修订）：per-plate
  支撑挤出体积（`GCodeProcessor support_volumes_per_extruder`，
  erSupportMaterial/Interface/Transition 三角色，普通/树/筏全覆盖）>0
  取反——配置层 `enable_support` 开启但模型自支撑时为 **true**。注意：输出
  gcode.3mf 内嵌盘元数据 `support_used` 仍为配置层（桌面 parity），与
  本 JSON 字段语义不同源，勿混用。
