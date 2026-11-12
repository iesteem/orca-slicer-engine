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

- **盘号一律 1-based**（内部 0-based + 1；日志里 `Starting slicing
  for plate M` 是 0-based，勿混淆）。盘号有两个键名：`plates[].id`（盘元素
  自身标识）与 `issues[].plate_id`（issue 所属盘的外键，-1=全局级保持原值）
- **`filaments[].id` 一律 1-based**（内部 extruder 0-based + 1）。**与
  `plates[].id` 是两个不同数域**：前者=耗材槽号（gcode T 编号 +1），后者=
  盘号，同名勿混淆

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
  "object_name": "Body1",             // 可为空串；TOOLPATH_OUTSIDE / TOOL_HEIGHT_OUTSIDE
                                      // 语义特殊：归因到的违规源——"wipe_tower"（合成
                                      // 名，塔不是模型对象）或对象名，多源英文逗号
                                      // 连接，空串=无法归因（TOOLPATH_OUTSIDE 如
                                      // bed_exclude_area 触发；TOOL_HEIGHT_OUTSIDE 如
                                      // 违规仅由 travel/z-hop 或自定义 G-code 承载——
                                      // 归因只喂挤出 move，非挤出 move 无对象归属）
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
  "id": 1,                            // 1-based 盘号（2.2.1 由 plate_id 改名）；
                                      // 注意与 filaments[].id（耗材槽号）数域不同
  "success": true,
  "issues": [ ... ],                  // 仅非空时输出该键（可选）
  // ---- 以下字段仅 success=true 时输出 ----
  "print_time_seconds": 1234.56,
  "print_time_formatted": "20h34m",
  "total_weight_g": 5.92,
  "filaments": [
    { "id": 1, "type": "PLA", "volume_type": "standard", "is_support": false, "color": "#FFFFFF", "used_g": 5.92, "used_mm": 1966.7 }
  ],
  // ↑ filaments[] 每条：id = 1-based 耗材槽号（gcode T 编号 +1，仅列该盘实际
  // 挤出的槽）；volume_type = 该槽流量类型声明，值域严格 {"standard",
  // "high_flow"}，恒输出（缺失/越界槽在装配层已坍缩为 "standard"）。取值源 =
  // post-apply print.config() 快照（capture_post_trim_config_snapshot）——
  // 与 gcode 头回显 "; filament_volume_type" 同源同值。**声明层语义**：
  // 它是驱动切片的声明，不是"高档实际生效"的证据——官方单档预设替换可把
  // 档位段塌缩回 standard 而声明仍是 high_flow（test_high_flow 塌缩用例
  // 钉死此行为）；生效判定须走 gcode 体四层法。App 一键打印按槽读此字段。
  // is_support = 该槽支撑耗材声明（耗材预设 filament_is_support），bool 恒
  // 输出（缺失/越界槽在装配层已坍缩为 false）。取值源与 volume_type 同一
  // post-apply 快照——与 gcode 头回显 "; filament_is_support" 同源同值。
  // 同为**声明层语义**：是预设/项目的声明，不是"实际挤出支撑"的证据
  // （后者是 support_free，gcode 实际层，刻意不同源，勿混用）。App 一键
  // 打印按槽读此字段（前端看不到引擎部署的耗材预设资源，无法自行判定）。
  "model_thumbnail": "<base64 PNG>",  // 可为空串
  "long_retraction_when_cut": false,
  "support_free": true                // gcode 实际层：该盘支撑挤出体积为 0（含配置
                                      // 开启但自支撑的场景；普通/树/筏支撑均计）
}
```

## 历史变更记录（防漂移）

| 版本 | 变更 |
|---|---|
| 2.2.1 (2026-11-11 追加) | **入参（破坏性，A3）**：C API params_json `"plate_id"` 仅接受 `0`（全切）；`> 0` 显式拒绝（SLIC3R_ERR_ARGS，消息含 "removed"/"all plates"，镜像 CLI `-p` 拒绝措辞）。盘选择在所有入口移除（引擎 single_plate 机制删除、`PLATE_NOT_FOUND` issue code 不再可达），引擎恒全切。输出侧不受影响（`plates[].id` / `issues[].plate_id` 为输出字段，照旧）。调用方通报由平台侧执行 |
| 2.2.1 (2026-11-11 追加) | **`TOOLPATH_OUTSIDE` 的 `object_name` 从恒空串改为归因越界源**：`"wipe_tower"`（erWipeTower 角色走，gcode 文本段名 "Prime tower"）或最近对象包围盒归因出的对象名，多源逗号连接；空串=无法归因（bed_exclude_area 触发等）。message 同步个性化（`Toolpaths of "X" extend...`）。**每盘仍恒一条 issue**（error_code 聚合按条数计数不变） |
| 2.2.1 (2026-11-11 追加) | **`TOOL_HEIGHT_OUTSIDE` 的 `object_name` 从恒空串改为归因超高源**：与 TOOLPATH_OUTSIDE 同核（`"wipe_tower"` 或最近对象名，多源逗号连接，归因只喂超高**挤出** move）；空串=违规仅由 travel/z-hop 或自定义 G-code（erCustom）承载。message 同步个性化（`G-code paths of "X" go beyond...`）。z_height 仍报实测 max_z。每盘恒一条 issue 不变。可达面注：模型超高主路径已被预处理 `BUILD_VOLUME_TOO_HIGH`（带名、阻断）覆盖，本条现实触发源以自定义 G-code 为主（Case 37 fixture 即此构造） |
| 2.2.1 (2026-11-11) | **`plates[].plate_id` → `id` 原地改名**（1-based 盘号，值域不变；filaments[].id 同名先例 v02.01.10）：对齐 filaments[].id 的元素标识命名。**同名不同义注意**：`plates[].id`=盘号、`plates[].filaments[].id`=耗材槽号，解析勿混淆。`issues[].plate_id`（全局 issues[] 与 plates[].issues[] 均含）**不改**——它是所属盘外键而非元素标识，保留描述性键名（-1=全局级）。C API 入参 params_json 的 `"plate_id"` 键不变（**后变更**：2026-11-11 追加行收紧为仅接受 0） |
| 2.2.1 (2026-11-10 新增) | `plates[].filaments[]` 条目内新增 **`volume_type`**（string，恒输出，值域 `{"standard","high_flow"}`，位于 `type` 之后）：该槽流量类型声明，取值源 = post-apply `print.config()` 快照（与 gcode 头回显同源；装配层对缺失/越界槽坍缩为 `"standard"`）。**声明层语义**——非"高档实际生效"证据（档位塌缩场景声明仍 high_flow），生效判定走 gcode 体。`print_info_total.filaments` 因 (type,color) 跨盘去重合并不加此字段（同组不同槽流量类型可不同）。App 一键打印契约字段 |
| 2.2.1 (2026-11-12 新增) | `plates[].filaments[]` 条目内新增 **`is_support`**（bool，恒输出，位于 `volume_type` 之后）：该槽支撑耗材声明（耗材预设 `filament_is_support`），取值源 = 与 volume_type 同一 post-apply `print.config()` 快照（与 gcode 头回显 "; filament_is_support" 同源同值；装配层对缺失/越界槽坍缩为 `false`）。**声明层语义**——非"实际挤出支撑"证据（那是 `support_free`，gcode 实际层，刻意不同源勿混用）。`print_info_total.filaments` 不加（(type,color) 去重原因同 volume_type）。App 一键打印契约字段（前端看不到引擎部署的耗材预设，无法自行判定） |
| 2.2.0 (2026-10-23 追加 P-35) | `SPIRAL_LIFT_NEAR_BOUNDARY` 触发条件精度化：判定源从机器域 `z_hop_types` 平键改为**有效值**（机器键 clone 后 `apply_override` 组合 `filament_z_hop_types`，镜像 PrintApply 合并语义）。code/level/消息不变，仅消除耗材覆盖已关掉螺旋时的误报（SnapSpeed Slope × 机器 Auto 基值场景实测两条误报→0） |
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
