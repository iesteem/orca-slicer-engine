# P-19: 空盘 rc=0 假成功（审查报告 C2，零产物退出码）

**状态：** 已修复（2026-09-20，时点未提交，工作区混有并行会话改动）。
源头：2026-09-16 云切片审查报告第四节 C2 → 2026-09-18 66-agent 对抗验证
标记 still_present → 2026-09-18 本地三场景运行时复现 → 2026-09-20 修复。
行为语义经用户决策：**选项 A（桌面 parity，良性跳过）**。

## 现象（修复前，CLI 实测）

| 场景 | rc | 产物 | JSON success | JSON 中空盘痕迹 |
|---|---|---|---|---|
| 单盘模式请求空盘（`-p 2`） | **0** | **无** | false | 零（无原因字段） |
| 多盘 1好+1空 | 0 | 有（好盘 gcode.3mf） | true | **零**（仅日志） |
| 多盘全空 | **0** | **无** | false | 零 |

rc=0 + 零产物 = 假成功：调用方以 rc 做流程分支（C API 契约
`slic3r_c_api.h:33` "returns engine.exit_code() directly"；CLI `main.c:302`
仅在 rc!=0 时报错），无法区分"成功有产物"与"什么都没发生"。

## 根因链（六环节，逐环代码实证）

1. **空盘标记无标志**：`mark_printable_instances`（修复前
   SliceEngine.cpp:2121-2144）count==0 → 仅 `BOOST_LOG_TRIVIAL(warning)
   << "Skipping empty plate"` + `return false`——不设 `m_any_error`、不设
   `m_error_type`、不推任何 issue。
2. **早退无结果**：`process_plate` 在此返回 → 空盘永不进
   `finalise_plate_result` → `m_plate_results` 无条目。
3. **姊妹路径级别不够**：`prepare_plate_print` 的 `NO_PRINTABLE_OBJECTS`
   虽推 issue，但是 `make_warning`（`Types.hpp:61`）= warning 级。
4. **晚期提升门不认 warning**：`finalise_statistics`（:3925-3937）只把
   `error`/`serious_warning` 级全局 issue 提升为 `m_any_error` +
   `set_error_type`。
5. **退出码只看标志**：`exit_code()`（:3657）只读 `m_error_type` /
   `m_any_error`，二者均未设 → `EXIT_OK`。
6. **run() bool 不参与 rc**：`slic3r_c_api.cpp:284` 直接返回
   `engine.exit_code()`；`run()` 的 false 只用于 `set_error` 文案。

自基线 cac2f98 以来该路径仅经历重命名（`filter_instances` →
`mark_printable_instances`），无语义修复。

## 已证伪的推测（勿重走弯路）

- **"finalise_statistics 会兜住"——否**：提升门只认 error/serious_warning
  （见根因 4），warning 级静默通过。
- **"run() 返回 false 会映射非零 rc"——否**：见根因 6。
- **"多盘 1好1空 rc=0 是缺陷"——否**：桌面全盘切片同样跳过空盘，
  部分成功 + rc=0 是正确语义（用户决策选项 A）。缺陷面仅限零产物场景。
- **"顺手给空盘补个 warning issue 即可"——错，有陷阱**：
  `patch_orphan_plate_issues`（原 :3871-3897）对任何带真实 plate_id 的
  孤儿 issue **无条件**强建 `success=false` 占位盘 → 多盘 9好1空的整体
  `success` 被翻 false，而 rc 仍 0、产物照常打包——好心补观测反而制造
  三方矛盾状态。修复必须连同占位盘机制一起分级（见下）。

## 修复方案（选项 A：桌面 parity，良性跳过）

三条设计约束，分别对应三个修改点：

1. **不能一刀切设 `m_any_error`**：`m_any_error` 喂给 all-or-nothing
   打包门（`run()` :280）与单盘 GCODE 删除（:285），空盘就设它会把
   9/10 好盘拖成无产物。→ 守卫放在打包门**之后**，且只在"零产物"
   （`!has_output`）时触发；部分成功保持 rc=0。
2. **不遮蔽更具体的错误码**：`set_error_type` 取 max 语义，若零产物
   守卫无条件设 6，会把先前失败路径记录的 4（slicing）等改写成 6。
   → 仅当 `m_error_type == EXIT_OK` 才设码。
3. **观测 issue 不许翻语义**：warning 级孤儿 issue 不得触发占位盘。
   → `patch_orphan_plate_issues` 分级化。

## 修改内容

- `src/SliceEngine.cpp` `process_plate`（:2091-2100）：`mark_printable_
  instances` 返回 false 时推 warning 级 `EMPTY_PLATE` issue（带盘归因，
  消息含 1-based 盘号）。
- `src/SliceEngine.cpp` `run()`（:301-315）：打包/清理块之后的零产物
  守卫 `if (!has_output) { m_any_error = true; if (m_error_type ==
  EXIT_OK) set_error_type(EXIT_PREPROCESS_ERROR); }`。
- `src/SliceEngine.cpp` `patch_orphan_plate_issues`（:3989-4018）：
  error/serious_warning 孤儿才建 `success=false` 占位盘；warning/tip
  孤儿留在全局 issue 列表（plate_id 保留归因）。顺带删除不可达的
  find_if 死分支（`plates_with_results` 本就由 `m_stats.plates` 构建）。
- `tests/test_slice_e2e.cpp`：`has_plate_issue` 辅助 + Case 15/16/17
  （23 断言，均按 P-18 教训预清理输出路径）。
- `tests/fixtures/`：`slice_empty_plate_skip.3mf`（盘1=立方体、盘2=空）、
  `slice_all_plates_empty_fail.3mf`（两盘皆空）+ MANIFEST 条目。
  制法：`geom_above_bed_ok.3mf` 解包，仅编辑
  `Metadata/model_settings.config`——追加无 `<model_instance>` 子块的
  `<plate>` 块（plater_id=2）/ 删除盘 1 的引用，几何与配置层不变。
- `docs/requirements/exit-codes.md`：新增 `EMPTY_PLATE` 行；修正
  `NO_PRINTABLE_OBJECTS` 级别漂移（文档原标 error，代码自始为
  make_warning）；exit 0/6 语义补零产物守卫说明。

**行为变更连带（有意）**：`NO_PRINTABLE_OBJECTS`（apply 后零实例）在
多盘下不再把整体 `success` 翻 false。旧行为是 success=false + rc=0 +
产物照常打包的自相矛盾；新行为与选项 A 一致（该盘无贡献即良性）。
error 级路径（BUILD_VOLUME / SLICING_TIMEOUT / PLATE_NOT_FOUND 等）的
占位盘行为不变——Case 2/3/6/7/9/14 断言原样通过即为证。

## 验证证据

- **修复前复现**（2026-09-18，手工 fixture + CLI）：三场景 rc 全 0
  （本文开头表格）。
- **修复后 CLI 复验**（2026-09-20，`build-static/orca-slice-engine`）：
  - 单盘空 `-p 2`：rc=6，零产物，JSON `EMPTY_PLATE` warning 归因
    plate_id=2，success=false；
  - 多盘 1好1空：rc=0，success=true，好盘 gcode.3mf 照常，`EMPTY_PLATE`
    归因可见；
  - 多盘全空：rc=6，零产物，两条 `EMPTY_PLATE` 各归因其盘。
- **测试**：`./engine-integration "[empty_plate]"` → 3 用例 23 断言
  全绿；全量 115 用例 / 620 断言零回归。
- 编译零新增警告（仅 deps 既有 TBB ODR 警告）。

## 遗留问题（识别、未处理）

1. **`NO_PRINTABLE_OBJECTS` 新行为未运行时触发**：需构造"实例已标记
   printable 但 `Print::apply` 后归零"的 fixture，当前无法制；其 benign
   化由与 `EMPTY_PLATE` 同一分级机制保证（代码级推断，非实测）。
2. **`error_message` 不进 JSON**：`JsonReport.cpp` 无该字段，零产物时
   JSON `success=false` 无原因字符串（理由只在 ctx error / CLI stderr /
   日志）。属独立 schema 议题，按 json-output.md 契约另行决策。
3. **未提交**：修复与并行会话工作（`SliceEngine.hpp`、
   `test_high_flow.cpp`、precedents P-17/P-18 等）交织于同一工作区，
   提交时需按 hunk 拆分（P-17 的 awk + `git apply --cached` 先例）。
4. **空盘在 `plates[]` 无条目**：观测走全局 issues（带 plate_id），
   `plates[]` 只含真实切片的盘。若未来调用方要求 plates[] 覆盖全部
   m_plate_data 盘，需再决策（受本判例分级语义约束）。
