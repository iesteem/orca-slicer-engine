# P-17: nozzle_volume_type 被官方打印机预设整体覆盖重置

**状态：** 已修复（`3640a49`，2026-09-20）。源头：2026-09-18 云切片审查
报告缺陷 2（31/37 输出重置）。同一报告的缺陷 1（brim 回显）见 P-11。

## 现象

输入工程 `nozzle_volume_type = high_flow`（每喷嘴一槽的枚举向量），输出
工程与 gcode 头部回显均为 `standard`——31/37 盘被重置，其余 6 盘本就是
standard。实际打印运动无影响（37/37 与基线运动学等价，报告实测）。

## 根因链（五环节，逐环代码实证）

1. **数据源**：官方 machine 预设 JSON 不含该 key。子预设
   `Snapmaker U1 (0.4 nozzle).json` 有 `nozzle_type`/`nozzle_volume`（不同
   key），父 `fdm_U1.json` 亦无；整个 `package/resources/profiles/Snapmaker/`
   grep 零命中。该 key 语义是"用户安装了什么喷嘴"的**硬件状态**，预设
   体系按直径分档、不按流量档分档，天然无法承载。
2. **加载期补默认**：fork `Preset.cpp` `Preset::normalize` 对 printer 配置
   把 `printer_options()`（⊇ `nozzle_options()` = `extruder_option_keys()`
   ∋ 该 key，`PrintConfig.cpp` `init_extruder_option_keys`）缺失的 key 从
   C++ 默认 clone → `{fvtStandard}`。
3. **槽位对齐**：`set_num_extruders` 按 `nozzle_diameter` 槽数规整向量
   （父 5 槽 → 5×standard，子 4 槽 → 截成 4×standard）。
4. **覆盖执行**：`SliceEngine::apply_printer_official_preset()` 的
   `overwrite_all_keys_from(m_config, official->config)`——vector 分支
   **逐槽全量复制**，不保留任何用户值。
5. **回显**：被覆盖的 `m_config` 经 `append_full_config`（fork
   `GCode.cpp:6754`，`; key = value` 注释块，黑名单仅 8 个凭据类 key）
   与输出 3mf `project_settings.config` 双写回，`standard` 落盘。

**为何 filament_volume_type 幸存（不对称性）**：三条替换路径覆盖语义
不同——打印机路径整体覆盖无保留；工艺路径
`overwrite_all_keys_from_except` + user_overrides；耗材路径
`PresetRollback::overwriteExtruderFrom` 逐槽只写官方预设实际定义的 key
（官方耗材 JSON 无 `filament_volume_type`）。

## 危害定性（对审查报告的修正）

报告称"一旦流量需求 > 66 mm³/s 该重置会真实限速"——**方向存疑**：切片
核心没有任何 `nozzle_volume_type` 读者。变体数组选列函数 `get_config_idx`
（fork `PrintConfig.cpp:7408-7427`）对 Process/Printer/Filament 三域全部
按 `filament_volume_type` 选列；`machine_flow_variant_options()` 为空
列表；`get_nozzle_volume_type`（fork `PresetFlowVariant.cpp:5`）调用方全
在 GUI（校准向导/Plater 校准流程）。`Print.cpp:680` 失效集收录它只是
保守捆绑。**该重置是纯回显/写回保真损伤**；真实风险在下游桌面重开输出
工程时校准向导/流量上限指示读错列（桌面 `Plater.cpp:18424` 模式），以及
未来切片核心一旦消费该 key 即变真实限速。

## 修复方案与修改内容

引擎侧快照嫁接：整体覆盖前快照用户值，覆盖后按官方槽位数 `std::copy_n`
嫁接回官方向量前 min(用户槽数, 官方槽数) 槽——官方预设决定槽数，用户值
填内容，槽位错位与跨槽污染构造性不可能。不做数据侧修复（往 machine JSON
写该 key 是范畴错误：预设不知道用户硬件）。

- `src/SliceEngine.cpp` `apply_printer_official_preset()`：+24 行
  （快照 + 嫁接，含根因注释）
- `tests/test_high_flow.cpp`：新增用例 "Nozzle volume type survives
  printer preset substitution"，fixture `high_flow_whitelist.3mf`
  （槽 0=high_flow、槽 1-3=standard，同时钉住保留与不误染两个方向），
  断言 `; nozzle_volume_type = high_flow,standard,standard,standard`
  及 `filament_volume_type` 不受影响；含产物预清理（原因见 P-18）。

## 验证证据

- 红：`git stash` 撤修复重编 → 单跑失败（头部 standard×4）。
- 绿：恢复重编 → 6 断言全过。
- 套件：engine-integration 114 用例 / 609 断言全绿。
- 提交为部分暂存（工作区混有并行会话空盘防护工作）：awk 按 hunk 过滤
  + `git apply --cached`，暂存变体（HEAD + 仅本修复）用真实构建验证可编译。

---

# P-18: 输出路径被占用时静默换名（时间戳后缀）

**状态：** 已按"只补可观测性"决策修复（`af80f0b`，2026-09-20）。
换名行为本身是有意设计，保持不变。该问题在修 P-17 的调试过程中暴露。

## 现象

输出路径已存在时（典型：上一次失败任务遗留产物），引擎 rc=0 报成功，
但产物写到 `<base>.gcode_<时间戳>.3mf`，契约路径上的旧文件原地不动。
调用方读契约路径会**静默拿到旧盘 gcode**。CLI 直连探针实证：

```
[info] gcode.3mf package created: /tmp/probe_stale.gcode_1789876627430320235.3mf
```

毒化链：测试 REQUIRE 中止在自身清理之前 → 失败一次即遗留产物 → 此后
每次运行都被静默换名 → 永远读到旧内容。本会话为此消耗约 45 分钟调试。

## 根因

`generate_output_path`（`src/Utils.cpp:145-151`）：目标已存在 → 追加
时间戳后缀，注释标明 "Prevent multi-process collision"（init 仓库即有；
服务场景含 single_plate + output_base 时任意 plate_id 生成同一路径的
per-plate 并行）。JSON 其实**一直上报真实路径**
（`print_info_total.output_file` ← `engine.output_path()`；顶层与 plates[]
 无此字段，勿 probe 错层级），但除日志外无任何换名信号。

## 已证伪的推测（勿重走弯路）

- **"store_bbs_3mf 追加/沿用旧条目"——错**：读码证实写出器写全新
  `.tmp` 再原子 rename（fork `bbs_3mf.cpp:5724-5734`），干净正确。
  换名发生在更早的路径生成层。首个修复提交的测试注释曾据此写错归因，
  已在 `af80f0b` 修正。
- **"JSON 不上报真实路径"——错**：`print_info_total.output_file` 一直
  携带；最初 probe 查了顶层/plates 层级得出 None 的错误结论。

## 修复方案与修改内容（用户决策：三选一中的"只补可观测性"）

行为不变，补两个透明度缺口：

- `generate_output_path` 增可选 `bool* relocated = nullptr` 出参（默认
  不影响既有调用方；`slic3r_c_api.cpp` 日志路径推导处未传）。
- `run()` 换名发生时发 task 级 `OUTPUT_PATH_RELOCATED` warning，消息带
  实际产物路径（`plate_id=-1`，不触发 `patch_orphan_plate_issues` 的
  占位盘 fabrication）。
- 修正 P-17 测试的错误归因注释；新增用例 "Output path relocation is
  surfaced, not silent"（fixture `u1_preset_test.3mf`）：预占契约路径 →
  断言 rc=0 + 产物在上报路径 ≠ 契约路径 + 旧文件原样未动 + warning 在
  issues 中；预清理扫掉本测试的历史时间戳遗留。

## 验证证据

- 红：临时禁用告警发射重编 → 失败于 `warned` 断言（5/1）。
- 绿：恢复 → 6 断言全过（620 断言套件内同）。
- 暂存变体（HEAD + 仅本修复，不含并行工作）独立构建 + 测试通过。
- 套件：engine-integration 115 用例 / 620 断言全绿。

## 调试工具坑（Catch2）

测试名含逗号时，按名过滤会被 Catch2 按逗号拆成两个 filter →
"No tests ran"。用 `*通配符*` 过滤。

---

# 遗留问题（本会话识别、未处理）

1. **审查报告缺陷 3 未修**：`printer_settings_id = "MyToolChanger 0.4
   nozzle - Copy"` 脏值烧在打包文件
   `package/resources/profiles/Snapmaker/machine/Snapmaker U1 (0.4 nozzle).json:79`
   （4 个 nozzle 变体都脏），随同一次整体覆盖带入。
2. **高流量咬合用例未补**（审查报告建议 4）：缺
   `filament_max_volumetric_speed > 66` 的用例让喷嘴档位真实咬合。
3. **日志路径发散**：`slic3r_c_api.cpp`（~:230）为推导日志路径独立重算
   `generate_output_path`——换名发生时两次调用时间戳不同，日志文件名与
   产物文件名不一致。未修。
4. **老用例预清理缺口**：`test_high_flow.cpp` 的 brim / high-flow 两个
   用例未加产物预清理（仅 nozzle 与 relocation 用例有），一旦失败一次
   即落入 P-18 毒化链。建议顺手补齐。
5. **换名行为语义本身保持现状**：云端 worker 若复用输出路径，须读
   issues 中的 `OUTPUT_PATH_RELOCATED` 或 `print_info_total.output_file`
   获取真实产物路径（契约义务在调用方）。
