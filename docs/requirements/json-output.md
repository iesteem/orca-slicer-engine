# 需求：JSON 输出契约

> 来源实现：`src/JsonReport.cpp`（`build_statistics_json`）。本契约是引擎
> 对外最不可漂移的接口——下游解析直接依赖，任何字段变更都是 breaking
> change，需人类批准并记 CHANGELOG。
> **[anchor] 级契约**：整体重构时必须保持逐字段兼容。

## 输出位置

- stdout：包裹在 `=== SLICE STATISTICS (JSON) ===` / `=== END STATISTICS ===` 之间
- 文件：`-j`/`--json` 指定路径（写入失败仅 warning，不影响退出码）。
  **显式路径无条件写**（P-28，2026-10-19）：调用方给了 json_output_path
  （CLI 显式 `-j`、裸 `-j` 预填、或不传 `-j` 时 main.c 的 `<output>.json`
  预填）即落盘——**含引擎执行过的早期失败出口**（rc=3/6 等，此时引擎尚
  未解析输出路径）。仅"未传 json_output_path 的 C API 直调方 + 失败运行"
  不落盘（2026-10-13 防垃圾文件决策收窄至此场景；此前该守卫误伤一切
  显式路径）
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
  "exit_code": 0,                     // int；进程/C API 返回码的原样镜像（SliceEngine::
                                      // exit_code() 在 build_statistics() 尾部盖章）。
                                      // 恒输出。set_error_type 单调棘轮原样发射——值可
                                      // 高于 issues 表面暗示的档位（TOOLPATH_OUTSIDE 记 7
                                      // 后，后续 6-stamp 是 no-op），**禁止从 issues 反推**。
                                      // 值域 {0,3,4,5,6,7}；rc=1（参数错）/rc=99（异常
                                      // 逃逸）**不产出任何报告**——文件缺席 = 无报告，
                                      // 绝非 exit_code 1/99（文件通道缺席的其余场景见
                                      // "已知语义注意点"末条，P-28）
  "error_code": "",                   // string；恒输出，成功为空串（确定值"无错误"，
                                      // 非未知，故不属"缺席=未知"家族）。内容 = 全部
                                      // level 为 error / serious_warning 的 issue 的
                                      // code，按全局 issues 列表序（finalise_statistics
                                      // 已稳定排序：error 先于 serious_warning）英文
                                      // 逗号拼接，**不去重**（每 issue 一项；重数细节
                                      // 在 issues[]）。聚合源仅全局 issues 列表（它是
                                      // 超集，plate issue 经 assemble_plate_stats 汇入；
                                      // 再走 plates[] 会把 orphan issue 重复计入）。
                                      // 不变式：success ⟺ exit_code==0 ⟺ error_code==""
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
| 2.2.0 (2026-10-19 修订) | **`error_message` → `error_code` 原地改名**（同日二次修订，2.2.0 未发布，support_free 改名先例）：内容从"与 slic3r_get_error 同串的人类可读摘要"改为"全部 error / serious_warning issue 的 code 英文逗号拼接（issues 列表序，不去重）"——旧摘要在 rc=6/7 恰好为空或泛化，聚合 code 精确归因。不变式改述为 success ⟺ exit_code==0 ⟺ error_code==""（全路径审计：所有 rc≠0 的 run() 出口均伴随 error/serious_warning issue，P-23 NO_PLATES_PRODUCED 已封掉最后一个无 issue 缺口）。C API `slic3r_get_error` 通道**不变**（仍返回结构体 error_message 摘要），JSON 不再镜像该串 |
| 2.2.0 (2026-10-19 追加 P-28) | **文件通道**：显式 json_output_path 无条件落盘（含 rc=3/6 早期失败；CLI 因 main.c 预填而恒落盘）；2026-10-13 防垃圾文件决策收窄至"未传路径的 C API 直调方失败运行"。裸 `-j` 对齐为预填 `<output>.json`（原发空路径=只打印不落盘，与帮助文本矛盾）。报告字段与不变式不变 |
| 2.2.0 (2026-10-19 追加) | 顶层新增 `exit_code`（int，恒输出：进程/C API 返回码原样镜像，棘轮语义勿从 issues 反推，值域 {0,3,4,5,6,7}，rc=1/99 无报告）与 `error_message`（string，恒输出：成功为空串、失败为人类可读摘要、与 slic3r_get_error 同串）。不变式 success ⟺ exit_code==0 ⟺ error_message==""。同时关闭 empty-plate-zero-product-rc.md 遗留项 2（"error_message 不进 JSON"） |
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
  调试开关 `--keep-failed-gcode3mf`（2026-10-20）会在 rc=7 时把 gcode.3mf
  落盘，但**不改变本契约**：`output_file` 仍为 ""（产物是取证用，非交付）。
- 无 plate 元数据的 3MF（第三方导出）按桌面 Orca 语义合成单盘切全部
  printable 实例，并发 tip 级 `PLATE_DATA_SYNTHESIZED`（云端可用其区分
  "真单盘文件"与"无盘文件被合成"）；合成盘无缩略图源，
  `model_thumbnail` 为空。
- `support_free` / `mixed_filament_used` / `resources_version` 均为
  **可选字段，缺席 = 未知**（各自输出条件见上文结构定义），下游禁止按
  "缺省即 false / 空串"读取。`support_free` 取反极性（2026-10-10 由
  `support_required` 改名翻转，未发布前原地修订）：有利状态（true=无需
  支撑）只能由显式 true 到达，缺席/误读缺省全部坍缩到保守侧"有支撑"。
- `exit_code` / `error_code` 与上述家族**刻意相反**：恒输出，空串/
  0 是确定值而非未知（空 error_code = "确无错误"）。下游读 verdict
  无需 contains() 判断。**文件通道**缺席（无 JSON 文件）只发生在：rc=1
  （参数错，含 CLI 侧）、rc=99（异常逃逸），或"未传 json_output_path 的
  C API 直调方 + 失败运行"（P-28 收窄后的 2026-10-13 防垃圾文件决策）；
  CLI 传了路径（含预填）则任何引擎执行过的失败都落盘。文件缺席时
  stats_out（C API 进程内）通常仍有完整报告——缺席 = 该通道无报告，
  应回退读进程返回码与 `slic3r_get_error`，**不得**当作 exit_code=1/99
  或空 error_code 解析。
- `support_free` 判定源是 **gcode 实际层**（同日二次修订）：per-plate
  支撑挤出体积（`GCodeProcessor support_volumes_per_extruder`，
  erSupportMaterial/Interface/Transition 三角色，普通/树/筏全覆盖）>0
  取反——配置层 `enable_support` 开启但模型自支撑时为 **true**。注意：输出
  gcode.3mf 内嵌盘元数据 `support_used` 仍为配置层（桌面 parity），与
  本 JSON 字段语义不同源，勿混用。
