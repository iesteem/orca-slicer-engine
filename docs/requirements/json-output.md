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
  ]
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
  "long_retraction_when_cut": false
}
```

## 历史变更记录（防漂移）

| 版本 | 变更 |
|---|---|
| v02.01.10 (2026-06-11) | `plates[].filaments[].filament_id` → `id`。libslic3r 内部 `FilamentInfo::filament_id` 不变 |

## 已知语义注意点

- 失败盘的 `print_time/weight/filaments` 字段**不输出**（非零垃圾值不可
  断言，见 test_slice_e2e 的写测试点）——下游必须按"键缺失 = 无数据"
  处理，而非默认 0。
- `plate_count` 计**所有**盘（含失败），`print_info_total` 聚合**仅成功盘**
  ——两者口径不同。
- all-or-nothing 打包（P-05）下，任一盘失败 → `success=false` 且
  `output_file=""`，即使其余盘切片成功（其 per-plate 数据仍在 plates[] 里）。
