# 2.2.0 调用方 Breaking Changes 通报与迁移指南

- **发布版本**：feature-2.2.0（`ENGINE_VERSION = 2.2.0`）
- **基线**：package-02.01.06（2026-09-14 构建）
- **适用对象**：云切片引擎的 CLI 调用方、C API（动态库）调用方、服务编排/运维
- **配套工具**：`tools/audit_caller_args.py`（生产 argv/params 审计，见 §五）
- **详细根因**：TECH-DESIGN-2.2.0-2026-10-10.md §2.3.3/§2.3.4/§2.4.1/§2.2.1

> 4 项变更会改变调用方可观察行为（rc / 产物 / JSON）。全部为设计文档记录的
> **有意变更**，但外部调用方升级前必须逐项核对。另见 §四"需知晓的非
> breaking 变更"。

---

## 一、TL;DR 迁移表

| # | 变更 | 升级后后果 | 迁移动作 |
|---|---|---|---|
| B1 | CLI `-p/--plate` 移除 | 传参即 **rc=1** 整单失败 | 删参数；单盘走 C API `plate_id` |
| B2 | 数值/format 参数严格校验 | `-t abc`、`-t 1.5`、format 大写、负值 → **rc=1**（06 静默容忍） | 全串非负整数；format 精确小写 `gcode`/`gcode.3mf` |
| B3 | C API `plate_id` 语义激活 | 已传 `plate_id>0` 的调用方**产物/退出码/JSON 全变** | 核对单盘产物的 plates[] 形状与 rc=6 分支 |
| B4 | 空盘不再翻 `success=false` | 依赖 `success==false` 检测空盘的调用方**静默漏检** | 改读 `issues[]` 的 `EMPTY_PLATE`/`NO_PRINTABLE_OBJECTS` |

---

## 二、逐项详情

### B1：CLI `-p/--plate` 移除（b739c20）

- **06 行为**：`-p N` 被静默忽略，实际**全盘切片** rc=0（单盘请求被放大 N 倍工作量，产物名/退出码语义随之错误）。
- **2.2.0 行为**：显式拒绝，`rc=1`，stderr 指引：
  `Error: option -p/--plate has been removed; ... (plate selection remains available via the C API params_json "plate_id" key)`
- **为何显式拒绝而非静默忽略**（对"未知选项忽略"策略的记录在案例外）：继续忽略会把单盘请求静默放大成全盘。
- **迁移**：删除 `-p`/`--plate`。单盘需求改走 C API `params_json` 的 `"plate_id"` 键。
- **无回退路径**：CLI 恒全切，该行为不再变化。
- **特别核对**：若调用方在 06 下已经"适配"了全盘产物（按全盘包取产物），升级后必须同步改造为单盘流或去掉 plate 语义。

### B2：数值与 format 参数严格校验（95a2246，REQ-07/REQ-10）

CLI 与 C API 双层生效。06 容忍、2.2.0 拒绝（`rc=1`）的完整输入对照：

| 输入 | 06 行为 | 2.2.0 行为 |
|---|---|---|
| CLI `-t abc` / `10x` / `0x10` | atoi→0，**静默无超时** | rc=1（报参数名） |
| CLI `-t 1.5` | atoi→1，部分生效 | rc=1（strtol 遇 `.` 全串校验失败） |
| CLI `-t -5`、`--max-size -3` | 静默跳过（>0 判断） | rc=1 |
| CLI `-t " 10"`（前导空格） | atoi 容忍 | rc=1 |
| CLI `-f GCODE` / `Gcode` / 任意串 | 非静默映射：`!= "gcode"` 即按 gcode.3mf | rc=1（**大小写敏感**白名单） |
| C API `{"plate_id":-1}` | 照收 → 实际全切 rc=0 | rc=1（报字段名） |
| C API `{"timeout_seconds":-5}` / `{"max_size_mb":-2}` | 静默无限制 | rc=1 |
| C API `{"format":"GCODE"}` 等非白名单 | 静默映射 gcode.3mf | rc=1 |

仍容忍项（两版一致，无需迁移）：`-t 0`（=无超时）、`+10`（strtol 接受）、
`plate_id=0`（=全盘）、`.3MF` 大写扩展名。

- **迁移**：编排侧生成参数处对数值做全串非负整数校验；format 常量统一小写。
- **上线前必做**：对生产调用日志跑 `tools/audit_caller_args.py`（§五）——
  任何一处 sloppy 参数在升级后**首次调用即整单 rc=1**。

### B3：C API `plate_id` 语义激活（95a2246 步骤 1；06 缺陷：C API 层从不赋值）

| plate_id 输入 | 06 行为 | 2.2.0 行为 |
|---|---|---|
| 缺席 / `0` | 全盘 | 全盘（无差异） |
| `1..N` 有效非空盘 | **静默全切**（plates[]≈N 盘、产物=全盘包、rc=0） | 仅切该盘（plates[]=1；`format:gcode` 直写 `.gcode`；3MF 单盘包） |
| `> 实际盘数` | 全切 rc=0（校验从不触发） | `PLATE_NOT_FOUND` error，**rc=6**，plates[] 空，零产物 |
| 指向空盘 | 全切 rc=0（好盘照常打包） | **rc=6** 零产物 + `EMPTY_PLATE` warning 归因该盘 |
| 数组/字符串类型 | rc=1 | rc=1（无差异） |

- **迁移**：盘点生产 params_json 是否已存在 `plate_id>0`——06 下这类请求每单
  都在做 N 倍无效功且读的是全盘包。升级后产物内容、plates[] 数量、退出码
  语义全变，需提前适配。产物命名：`output_base` 为空时单盘产物带 `-pN`
  后缀（`<stem>-pN.gcode`）。
- **新增硬失败分支**：越界/空盘 rc=6 是 06 不存在的失败模式，调用方重试
  逻辑需识别（见 exit-codes.md"盘选择与归属"组）。

### B4：空盘不再翻 `success=false`（6aa4d6b，P-19 选项 A）

- **06 行为**：多盘任务中任一空盘（`NO_PRINTABLE_OBJECTS`/`EMPTY_PLATE` 场景）
  经 `patch_orphan_plate_issues` 无条件伪造 `success=false` 占位 → 整体
  `success=false` 但 **rc=0 且产物照打包**（三方矛盾状态）。
- **2.2.0 行为**：空盘发 warning 级 issue（带 plate_id 归因）良性跳过；
  **仅零产物**时 rc=6。9 好 1 空仍是 `success=true` + rc=0 + 产物照常。
- **迁移（重点）**：任何用 `success==false` 判断"某盘未产出"的调用方逻辑
  现在会**静默漏检**——改读 `issues[]`：
  - `EMPTY_PLATE`（warning，plate_id 归因）= 盘被跳过
  - `NO_PRINTABLE_OBJECTS`（warning）= 盘无对象
  - `NO_PLATES_PRODUCED`（error，全局）= 零产物兜底（rc=6）
- **rc 分布变化**：全空/单盘请求空盘从 rc=0 → rc=6，rc=6 占比将上升，
  告警阈值需相应调整。

---

## 三、静默行为变更（不改变 rc，但产物内容不同）

| 项 | 06 | 2.2.0 |
|---|---|---|
| `--skip-preset-substitution`（CLI）/ `skip_preset_substitution` 键（C API） | 跳过预设替换 | **静默忽略**——预设替换 + 用户 G-code/notes/外链剥离无条件执行（f57fb1f；参数在 06 下本就暴露于预设缺失必崩面，见判例 P-09） |
| 预设装载失败 | 部分场景带默认值继续出产物 | **硬门控** rc=6 + `PRESETS_MISSING` error（95a2246）。资源目录配错/权限/磁盘满从"降质成功"变整单失败——运维需监控该码 |
| 官方预设覆写 | 逐元素 min 拷贝（短侧截断、混合配置） | 整体覆写 + 槽位归一（P-21/e2e2ac9）——gcode 内容（床温/流量/换色冲刷矩阵）与 06 有差异，属修复非回归 |

---

## 四、需知晓的非 breaking 变更

- **新增 JSON 字段**（全部可选、缺席=未知，见 json-output.md）：
  - 顶层 `resources_version`：vendor profile 版本原样回显（如 `02.03.03.03`），
    失败出口也在场——排障时优先读它确认资源版本。
  - `print_info_total.support_free`：true=所有盘均未开启支撑（配置层判定，
    发射时取反；缺席坍缩保守侧"有支撑"）。**注意极性**：true 是"无需支撑"。
  - `print_info_total.mixed_filament_used`：输入文件是否引用虚拟混色槽。
  - `plates[].support_free`：同上，盘级。
  - 下游若有 `additionalProperties:false` 类严格 schema 校验需放行这些键。
- **新增 issue 码**：`NO_PLATES_PRODUCED`/`PLATE_MEMBERSHIP_REBUILT`/
  `INSTANCE_NOT_ON_ANY_PLATE`（exit-codes.md"盘选择与归属"组）。
- **输出路径搬迁可观测**：输出路径被占用时仍会搬迁写时间戳路径（行为
  不变），但现在发 `OUTPUT_PATH_RELOCATED` warning——以
  `print_info_total.output_file` 为准读真实路径。

## 五、生产 argv 审计（上线前必做）

```bash
# 1) CLI 调用日志（每行一条完整命令行）
python3 tools/audit_caller_args.py --cli-log /path/to/cli_invocations.log

# 2) C API params 日志（每行一条 params_json）
python3 tools/audit_caller_args.py --json-log /path/to/params_json.log

# 3) 扫调用方脚本仓库
python3 tools/audit_caller_args.py --scan /path/to/caller-repo
```

退出码 0=干净，1=存在 breaking 命中。规则 CR1-CR9 与本通报 §一/§二/§三
一一对应（脚本头部有映射说明）。`--strict` 把语义/静默变更也计入退出码。

**本仓库自审结论（2026-10-10）**：仓库内唯一引擎调用点为
`scripts/package_linux.sh:43`（`--help` 提取版本号），零踩雷。

## 六、上线后监控建议

| 指标 | 信号 | 阈值建议 |
|---|---|---|
| rc=1 占比跃升 | 参数严格化的漏网 sloppy 调用 | 升级后 24h 逐单排查，稳定后归零 |
| rc=6 占比 | 空盘/零产物/盘选择/预设缺失新语义 | 与升级前基线对比；`PRESETS_MISSING` 单独告警（运维问题非输入问题） |
| `FILAMENT_TEMP_MIXING` 出现率 | 混用检测收集面扩大的生产翻红率（修复本意，但需确认无合法文件误伤） | 首周逐单抽样 |
| 日志关键字 `brim echo restore` | 仅在 6 处降级路径出现；任何命中 = gcode 头回显说谎已发生 | 零容忍，命中即查 |
| `OUTPUT_PATH_RELOCATED` | 输出路径被占用 | 命中即清理遗留产物 |
