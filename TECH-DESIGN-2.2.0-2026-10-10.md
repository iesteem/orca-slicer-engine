# 云切片引擎 v2.2.0 技术方案（评审稿）

- **评审日期**：2026-10-10
- **版本**：feature-2.2.0 @ b26feca（`ENGINE_VERSION = 2.2.0`，CMakeLists.txt:23）
- **基线**：package-02.01.06（v02.01.06，2026-09-14 构建）；差分实测报告 `REVIEW-DIFF-020106-VS-220-2026-09-28.md`
- **来源**：2026-09-16《云切片引擎审查报告》13 REQ + 生产事故驱动（P-22/P-23）+ 对抗审查（2026-09-18/20 两轮）
- **支撑材料**：`docs/requirements/fix-requirements-2.2.0.md`、`docs/precedents/`（P-01..P-23 判例库）、`REVIEW-C4-C5-2026-09-20.md`

> 本文每节按统一结构：**问题 → 根因 → 候选方案对比（含被否决方案及其否决证据）→ 选型
> → 验证**。被否决方案不是凑数——全部来自判例库"已证伪/否决"条目，是真实走过或对抗
> 审查过的岔路，防止下一代实现重走。

---

## 一、总体设计原则（贯穿全部选型）

| # | 原则 | 出处 |
|---|---|---|
| D1 | **桌面 parity 准绳**：引擎与桌面行为不一致时，除有记录的偏离决策，一律视为引擎 bug | CHARTER.md |
| D2 | **数据层修复优于消费点分支**：载入后一次性补齐数据（入册/归一），让全部下游消费点自动接通，而不是在每个消费点加 if | P-22 D1'''、P-23 Fix A |
| D3 | **观测不翻语义**：补 issue 只用于可观测性，不得改变 success/rc/产物三方语义 | P-19 约束 3 |
| D4 | **退出码具体性**：更具体的错误码（如 slicing=4）不被通用码（preprocess=6）覆盖 | P-19 约束 2 |
| D5 | **all-or-nothing 打包门是宪法级约束**：任何修复不得把"部分盘失败"扩大成"整单无产物"（m_any_error 的设置点必须审慎） | P-05、P-19 约束 1 |
| D6 | **上游 bug 记录不盲修**：libslic3r/fork 缺陷记判例、可绕则引擎侧绕，绕不开才动上游 | CHARTER.md |

---

## 二、已实施方案

### 2.1 切片正确性

#### 2.1.1 REQ-01（审查 C1）：高低温混用检测漏检对象级/零件级/modifier 配置

**问题**：06 仅收集模型主体 `get_extruders()` + 全局键；对象级 feature 键
（wall/sparse/solid_infill_filament 等 6 键）、零件级键、modifier 的 extruder 指定
高温耗材不参与混用判定。实测 7 类场景全部 rc=0 放行——U1 工具切换器暴露在热冲击
风险下（主体 PLA + sparse_infill ABS 切片"成功"）。

**根因**：`check_filament_temp_mixing()` 收集面窄于桌面 Plater。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 镜像桌面 Plater 收集面（对象级主挤出机 + 6 个 per-feature 键 + 全部 volume 含 modifier + 每对象收集一次） | 桌面 parity（D1）；覆盖全部真实引用来源 | 收集面大，需防止误杀 | **采用**（cd14768） |
| B. 按 `filament_settings_id` 全槽判定 | 实现最简 | 闲置槽装载高温耗材即拦截——误杀合法文件 | 否决（`unused_abs_slot_ok` 用例钉死） |
| C. 收缩判定：只要出现 ABS 槽即全盘拒绝 | 保守安全 | 与桌面语义分叉，拒绝面过大 | 否决 |

**选型要点**：新 helper `collect_filament_slots_from_model_config` 镜像桌面
`Plater.cpp:461-484`；`is_model_part()` 过滤器删除（modifier 也算引用源）；盘级
config 源**不实现**——BBS 3MF 盘配置白名单不含 filament 键，云输入无法表达
（函数头注释记录）。仅按**实际使用**槽位判定。

**验证**：`[tempmode]` 16/16（修复前 7 红）；CLI 端到端对象级 fixture rc=6 +
FILAMENT_TEMP_MIXING 带盘归因；反误杀 2 用例保持 rc=0；全量零回归。

---

#### 2.1.2 REQ-04（审查 C3 / P-21）：官方预设覆写向量短侧截断

**问题**：06 覆写按 `min(dst,src)` 逐元素拷贝：用户 3MF 向量短于官方槽位数时仅
覆写短侧。实测 5 个 per-filament 向量卡 len=2、flush 矩阵 2×2、槽 2/3 悬空用户值
（官方+用户混合配置流入切片）。空向量（如 3MF 未携带 `compatible_printers`）
永远收不到官方值。

**根因链**：D1 整体覆盖逐元素 min 拷贝；D2 逐槽替换 `continue` 静默吞短向量；
D3 引擎无长度规整。**附带上游发现**：fork 的 `set_num_filaments()` 是彻底 no-op
（`PrintConfigDef` 构造漏调 `init_filament_option_keys()`，全树零调用者）——桌面
靠 GUI 层同名函数掩盖，引擎无 GUI 层必须自建。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. wholesale 整体覆写 + 以 `filament_settings_id` 槽数为权威归一化 | 桌面 `apply()` 语义；空向量也能接收官方值；槽数权威与桌面 Plater 同源 | 需自建 23 键规整表（上游 no-op 所迫） | **采用**（e2e2ac9） |
| B. 直接调 `Preset::normalize` | 零自建代码 | **已证伪**：它从 `filament_diameter.size()` 取 n，而截断 3MF 恰污染该键 → 整项目错切 2 槽 | 否决（实测证伪） |
| C. 只修 D2（短向量警告 + set_at 增长） | 改动最小 | **已证伪**：温度键是 flow-variant 分段存储，`overwriteExtruderFrom` 刻意跳过，修的是另一个维度；D1/D3 仍在 | 否决 |
| D. 修上游 fork（补 `init_filament_option_keys()` 调用） | 治本 | 跨仓库、周期长；桌面 8 仓联动回归 | 列入上游上报清单（见 4.5），引擎侧 A 先行 |

**选型要点**：`normalize_filament_vectors_to_slot_count()` 两调用点幂等挂接
（strip_user_content 后、printer wholesale 后）；核心键 23 项精确 resize
（grows AND shrinks，flow-variant 键跳过）；`set_num_extruders` /
`nozzle_diameter` 刻意不移植（挤出机数归官方机预设所有）。**有意超越桌面**处：
超长向量收缩到 n（与桌面 GUI 层 `PresetBundle::set_num_filaments` 行为一致）。

**验证**：红→绿 21 断言（6 键 len 2→4、flush 2×2→4×4、槽 2/3 官方值）；
差分实测产物 `project_settings.config` 全向量 len=4 且值=官方。

---

#### 2.1.3 REQ-05（P-11 / P-20）：auto_brim 裙边宽度回显保真

**问题**：两阶段缺陷。①2026-06：`brim_type=auto_brim` 时引擎画裙边而桌面不画
（fork fallback 语义差异）；②第一轮修复把清零作用于 `m_config` 单一对象，而它同时
喂切片（要 0）、gcode 头回显（要 5）、输出 3mf 配置（要 5）→ 回显说谎
`brim_width = 0`（37/37 盘）。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 清零挪到切片副本（merged_config）+ export 后回显改写 `restore_brim_width_echo()` | print 输入逐位不变（运动零回归）；两个出口恢复真值 | 回显改写是文件级 I/O，需加固（见下） | **采用**（f291619 + b79415d） |
| B. 删掉清零逻辑 | 最简 | **已证伪**：fork fallback 生效，立方体/0.6/0.8 用例画出 5.13mm 裙边而桌面不画——运动回归 | 否决（skip 实验实证） |
| C. 修 fork `Brim.cpp` fallback 语义对齐桌面 | 治本 | fork 侧 `brim_width` 消费点多（面积通道、PrintObject.cpp:698），语义复制风险高 | 否决（判例记录风险） |

**Stage 2.1 加固**（对抗审查发现 1×P1 + 3×P2）：close 后流状态校验（I/O 失败保留
原文件，防截断 tmp 静默覆盖完好 gcode）；整体 try/catch 降级 warning（注释行改写
失败不得放大为整单 GCODE_EXPORT_ERROR 报废）；`TmpFileGuard` RAII（防 `.brimecho.tmp`
泄漏进输出目录）；CONFIG_BLOCK 区间锚定 + 旧值校验为 "0"（防不受信 3MF 透传
machine gcode 劫持首行匹配）。

**Stage 3 补漏**（2026-10-10 发布前回归审查，唯一 N 级 FAIL）：`expected` 字符串
拼接原在 try 块外，bad_alloc 逃逸仍会放大为整单 GCODE_EXPORT_ERROR，违背
never-escalate 承诺——三行挪入 try（纯代码移动，正常路径逐位不变；bad_alloc 路径
无 fixture 可触发，验证=编译 + `[brim]` 用例 10 断言复绿，2026-10-10 实测通过）。

**发布后监控**：日志关键字 `brim echo restore`。该串只在降级路径出现（warning/
error 级：gcode 打不开 / 复制 I/O 失败 / 锚定缺失 / rename 失败 / 异常捕获共 6 处），
成功路径日志前缀为 `brim_type=auto_brim:` 不含该串——**任何命中即回显说谎已发生**
（gcode 头留 `; brim_width = 0`，内嵌 config 与运动不变），需人工核对产物 gcode 头
并定位 I/O 层根因（磁盘满 / 权限 / 并发占用）。

**验证**：`high_flow_whitelist.3mf` 产物 gcode 头 `; brim_width = 5`（06 为 0），
运动行与 06 逐行一致；输出 3mf 内嵌 config JSON 断言 `"brim_width": "5"`。

---

#### 2.1.4 REQ-06（P-17）：nozzle_volume_type 被官方打印机预设整体覆盖重置

**问题**：官方 machine 预设 JSON 不含该键（语义是"用户装了什么喷嘴"的硬件状态，
预设按直径分档、天然无法承载）；wholesale 覆盖后 31/37 盘重置为全 standard。
**危害定性修正**（对审查报告）：切片核心无该键读者，`get_config_idx` 全部按
`filament_volume_type` 选列——纯回显/写回保真损伤；真实风险在下游桌面重开输出
工程时校准向导读错列。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 快照嫁接：覆盖前快照用户值，覆盖后按官方槽数 `copy_n` 嫁接回前 min(用户,官方) 槽 | 槽数归官方、内容归用户，错位构造性不可能；+24 行 | 每个此类键需逐个处理（当前仅此键） | **采用**（3640a49） |
| B. 往 machine JSON 补该键 | 数据侧自愈 | **范畴错误**：预设不知道用户硬件 | 否决（判例记录） |
| C. 白名单键跳过 wholesale | 通用 | 白名单维护成本；与"官方覆盖不可信用户值"的替换哲学冲突 | 否决 |

**验证**：槽 0=high_flow 产物 `["high_flow","standard","standard","standard"]`
（06 全 standard）；`filament_volume_type` 不受影响的反向断言同用例钉住。

---

#### 2.1.5 P-22：无 plate 元数据 3MF 假成功静默失败

**问题**：第三方导出 3MF（无 `<plate>` 块）：引擎兜底只补盘壳不补成员关系 →
`loaded_id` 恒 0 → 6 个匹配点全部落空 → "Skipping empty plate" 仅日志 →
**exit 0 + plates:[] + 零产物**——比崩溃更难发现的假成功。桌面版对同一文件切片成功。

**桌面语义（实证链）**：无 plate = 单盘 = 全部 printable 床内实例。

**候选方案**（两轮对抗审查淘汰后）：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| D1'''. 载入后数据层合成：造 `PlateData{plate_index=0}` + 每个 printable 实例入册（`loaded_id = id().id` + 递增 key） | 6 个匹配点零改动自动接通；`id().id` 与导出器 fallback 同源，全库 `(loaded_id>0)` 分支前后全等 | 合成盘需要标志位门控 error 化路径 | **采用**（21b4ef1） |
| D1 纯标志版（造盘 + 各消费点加"合成盘"分支） | 不动数据 | **证伪**：合成盘 identify_ids 恒空集，6 点逐一加分支漏一处即静默失效（首轮审查即杀） | 否决 |
| D3. 消费点"空 map = 全选" fallback | 改动集中在消费点 | **证伪**：无法区分"文件真没成员"与"盘真没对象"（桌面导出空盘即前者反例），会推翻 P-19 已决策语义 | 否决 |
| 升级 fork 导入器解析新格式 | 治本 | 超出引擎侧修复半径；桌面自己也不依赖该元数据 | 否决（non-goal） |

**配套**：合成盘零成员 → `PLATELESS_NO_PRINTABLE_INSTANCES` error（rc=6），
以 `m_synthesized_single_plate` 门控收窄；tip 级 `PLATE_DATA_SYNTHESIZED` 供云端
区分真单盘与合成盘；`validate_input()` 空盘兜底改硬不变量（堵死 exit-0 无产物通道）。

**验证**：黄金基准——桌面 2.4.0 同文件 gcode 重量 21.28g **逐位一致**、首条对象
打印移动逐字节一致；5 个真实无盘模型 4 个恢复产出；e2e Case 20-23。

---

#### 2.1.6 P-23：盘元数据无成员关系 → 假空盘零产物（MakerWorld 新格式）

**问题**：生产事故件（MakerWorld 下载、`BambuStudio-02.06.00.51`/`OrcaSlicer 2.4.2`
写出）：盘壳存在但 `<plate>` 块 **0 个 `<model_instance>` 子元素**（新格式只写
盘壳、摆放信息挪到 `<assemble>` 节）。引擎把 obj_inst_map 当唯一权威 → 0 可打印
实例 → 走 P-19 分支空盘跳过 → 唯一盘被跳 → 零产物。且该分支只写 `error_message`
不落 issue——**123 条 issue 全 warning，success=false，exit 6**：只读 issue 列表的
调用方把硬失败误判为成功。P-22 兜底守卫是 `m_plate_data.empty()`，本文件有盘壳
不触发。

**桌面语义（实证）**：`PartPlateList::load_from_3mf_structure` 的 obj_inst_map
恢复代码**已被注释废弃**（"we will rebuild later"），`reload_all_objects()` 几何
重算归属：凸包包围盒 ∩ 盘矩形（SceneEpsilon 膨胀），首个命中盘获胜。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| Fix A. 几何重建 `rebuild_plate_membership()`：仅 obj_inst_map 为空的盘为候选；逐 printable 实例凸包包围盒与盘格子矩形相交测试，首个命中入册（沿用 P-22 enroll 模式） | 桌面语义移植；有成员记录的盘行为原样（全部既有 fixture 零变化）；顺带绕开上游单键 map 多实例丢实例缺陷 | 与桌面已知分歧：实例跨候选/非候选盘格子时命中序可能不同（真实可达文件上等价——见判例论证） | **采用**（784c9ca） |
| 消费端"空 map = 全选"兜底 | 改动小 | **证伪**：P-22 文档已证伪的 D3 同族 | 否决 |
| 升级 fork 导入器解析 `<assemble>` | 治本 | 桌面自己也不靠该节判归属（assemble 处理器只回填 transform）；超出修复半径 | 否决 |
| 桌面式全量几何重建（每次载入重建所有盘） | 完全 parity | 改变全部既有盘归属来源（记录 vs 几何），回归面不可控 | 收敛为"仅空 map 盘候选"（**2026-10-10 修订：生产反例 2d986d79 证伪该收敛，经人类批准扩为 always-rebuild，见 P-24**） |
| Fix B 无条件 push NO_PLATES_PRODUCED | 可观测性最强 | 与 PLATELESS_NO_PRINTABLE_INSTANCES 双错误并列，破坏单一归因 | 加"已有 error 级 issue 则抑制"守卫（122c236） |

**验证**：事故件修复前 rc=6/全 warning → 修复后 rc=0、`PLATE_MEMBERSHIP_REBUILT`
tip、产物 3.67MB、重量 31.74g（内嵌黄金 21.04g，官方预设替换致偏，Case 26 以
5-40g 宽区间锚定）；e2e Case 26/27 + Case 17 更新；全量 780 用例绿（2026-10-08）。

---

#### 2.1.7 layer_config_ranges 手工注入 SIGSEGV（P0）

**问题**：`layer_config_ranges.xml` 导入零校验，libslic3r 的
`layer_height_profile_from_ranges` 无 null 检查解引用 `option("layer_height")`——
缺该键的 crafted/损坏 3MF 在 `Print::validate` 内 SIGSEGV rc=139（实测注入
fixture 复现；桌面同缺陷，fork `Slicing.cpp:184` 至今未修）。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 引擎侧 sanitize：缺 layer_height → 回退全局默认 + `CONFIG_INVALID_LAYER_RANGE` 告警；越界 filament id → 仅告警（libslic3r 已在挤出机收集处 clamp） | 修崩溃不改变合法数据行为；越界保留归因 | 引擎侧多一道防线（上游不修时的常态） | **采用**（0bf292a） |
| B. 拒绝携带缺陷 ranges 的整个文件 | 最强信号 | 过度：越界 id 是良性可 clamp 的；制造桌面没有的行为分叉 | 否决 |
| C. 只修 fork `Slicing.cpp:184` 补 null 检查 | 治本 | 跨仓库；桌面同修周期不可控 | 列入上游上报清单（4.5） |

**验证**：两注入 fixture（missing_height 修复前 SIGSEGV / oob_filament）rc=0 +
告警断言；`[layerheight]` 8 用例证明正常高度范围数据不受扰。

---

### 2.2 失败语义与退出码

#### 2.2.1 REQ-02（审查 C2 / P-19）：空盘假成功 rc=0

**问题**：三场景实测——单盘请求空盘 rc=0 零产物；多盘 1好1空 rc=0 且空盘在 JSON
零痕迹；全空 rc=0 但 success=false 自相矛盾。根因六环节（空盘标记无标志 → 早退
无结果 → 姊妹路径级别不够 → 提升门不认 warning → 退出码只看标志 → run() bool
不参与 rc）。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 桌面 parity 良性跳过：空盘发 warning 级 `EMPTY_PLATE`（带盘归因）+ 跳过；**仅零产物**时 rc=6；`set_error_type` 只在 `EXIT_OK` 时设（不遮蔽 slicing=4 等）；占位盘机制按 issue 级别分级（error/serious_warning 才建 `success=false` 占位盘） | 多盘部分成功语义正确（9好1空仍产出）；观测不翻语义 | 三处修改点需协同（打包门后守卫 / max 语义 / 占位盘分级） | **采用**（6aa4d6b，用户决策） |
| B. 任何空盘即整单 rc=6 | 调用方处理最简 | 一个空盘拖死 9 个好盘，违背桌面全盘切片语义与 D5 | 否决 |
| C. 维持 rc=0（06 现状） | 零改动 | 原缺陷：rc 无法区分"成功有产物"与"什么都没发生" | 否决 |
| D. 只补 warning issue 不动占位盘 | 改动最小 | **证伪陷阱**：`patch_orphan_plate_issues` 对带 plate_id 的孤儿 issue 无条件强建 `success=false` 占位盘 → 整体 success 翻 false 而 rc 仍 0、产物照打包——三方矛盾状态 | 否决（好心补观测反而制造新缺陷） |

**连带行为变更（有意）**：`NO_PRINTABLE_OBJECTS` 多盘下不再翻整体 success
（旧行为 success=false + rc=0 + 产物照打包本就自相矛盾）。

**验证**：三场景 CLI 复验（rc=0+EMPTY_PLATE 归因 / rc=6 零产物 / rc=6 双归因）；
e2e Case 15/16/17 共 23 断言；exit-codes.md 契约同步。

---

#### 2.2.2 REQ-11（P-18）：输出路径搬迁静默

**问题**：输出路径被占用（典型：上次失败任务遗留）时静默换写时间戳路径——
调用方读契约路径**静默拿到旧盘 gcode**。测试毒化链：REQUIRE 中止在自清理前 →
失败一次即永久读旧内容。

**候选方案**（三选一，用户决策）：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 只补可观测性：行为保留，`generate_output_path` 加 `bool* relocated` 出参 + task 级 `OUTPUT_PATH_RELOCATED` warning（plate_id=-1 不触发占位盘） | 不破坏多进程防碰撞设计（命名初衷）；JSON `print_info_total.output_file` 本就带真实路径 | 契约义务转移到调用方（须读 issue 或 output_file） | **采用**（af80f0b） |
| B. 搬迁即失败拒绝 | 语义最严格 | 误伤合法并发场景（single_plate 并行同路径）；"占用"非错误 | 否决 |
| C. 自动清理旧文件后原路径写入 | 调用方零感知 | 删除他人产物的破坏性语义；多进程竞态 | 否决 |

**验证**：预占位 → rc=0 + warning 在 issues + 产物在上报路径 ≠ 契约路径 + 旧文件
原样未动（6 断言红→绿）。

---

### 2.3 安全与健壮性（95a2246 批次，3N+7C）

#### 2.3.1 REQ-07（N1）：CLI→引擎 params JSON 构造注入与截断

**问题**：06 用 2048 缓冲 + `pos += snprintf`（**返回值误用**：截断时返回"本应
写入"的长度，pos 直接越过终点）——`cancel_file`/`log_path`/`json_output_path`
三字段走 `%s` 拼接，含引号即注入破坏 JSON（实测 rc=1 parse error）；路径超长截断
产生非法 JSON。**审查报告修正**：`input_3mf` 是独立函数参数不进 JSON，原 M6
"输入文件名注入"表述不成立。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 手写 buffer 升级：8192 容量 + bounds-checked `appendf` + `append_json_str`（字符串全部转义） | 零新依赖；最小侵入 | 手写序列化需逐字段纪律 | **采用** |
| B. 引入 JSON 库（nlohmann/json）序列化 | 类型安全 | 新依赖 + 全部构造点重写，收益不匹配 | 否决 |

**验证**：`--cancel-file '/tmp/c"x'` 引号 rc=0 正常切片（06 rc=1）；2186 字符
`-j` 路径 rc=0 且 stats.json 正常产出（06 截断 rc=1）。

---

#### 2.3.2 REQ-08（N2）：超短 PNG 缩略图堆越界读

**问题**：3 字节 PNG 文件直达 magic 比较，读取 8 字节签名堆越界读（无 ASAN 静默）。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 签名比较前置长度守卫（<8 拒绝） | 一行防线；缩略图尽力而为语义不变 | 无 | **采用** |
| B. 缩略图失败即整单失败 | 强一致 | 违背缩略图 best-effort 契约 | 否决 |

**验证**：`thumbnail_short_png.3mf`（3 字节）rc=0 + 日志 `skip plate 0, bad size 3`
（06 走 `bad PNG magic` 路径可区分）；ASAN 构建无 heap-buffer-overflow。

---

#### 2.3.3 REQ-09（N3 / P-09）：预设装载失败硬门控

**问题**：06 在 `--skip-preset-substitution` 下系统预设加载失败仍继续 →
`get_current_vendor_type` 解引用空配置 → **SIGSEGV rc=139 core dump**（所有输入
必崩）。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. `load_system_presets` 成功与否无条件门控后续流程；失败路径输出结构化 `PRESETS_MISSING` + rc=6 | 语义正确：skip 只该跳过替换，不该跳过可用性校验 | 无 | **采用**（95a2246） |
| B. 各解引用点逐个补 null 检查 | 就地防御 | 打地鼠；空预设状态下游处处是雷 | 否决 |

**后续（f57fb1f）**：`--skip-preset-substitution` 参数整体移除——调试价值已被
硬门控取代，保留只会诱导调用方进入无预设替换路径；传参静默忽略（与未知选项忽略
策略一致，门控行为等价）。判例 P-09 记录勿再建议用该参数调试。

---

#### 2.3.4 REQ-10：CLI/C API 边界参数校验

**问题**：06 用 `atoi` 解析数值（`-t abc`→0=静默无超时）、未知 format 静默按
默认执行、负 plate_id 照收。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 数值参数严格解析（strtol 全串校验，≥0）、format 白名单、API 边界与 CLI 双层生效 | 拒绝面明确 rc=1 | 无 | **采用** |
| B. 容错归默认继续 | 调用方零适配 | 静默语义漂移（无超时=无限等待生产事故面） | 否决 |

---

#### 2.3.5 REQ-12：日志 sink 生命周期（☆ 代码级，CLI 不可见）

**问题**：06 重复添加文件/控制台 sink，同进程多次任务（长驻 DLL 模式）日志重复
与句柄残留。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 添加文件 sink 前移除既有；控制台 sink 与公共属性去重 | 就地修复 | — | **采用** |
| B. 全局 sink 单例重构 | 架构更净 | 改动面大，收益不匹配 | 否决 |

**验证缺口**：需 C API 同进程连续两次切片断言无重复行/无句柄泄漏——**未做**，
列入 4.9 待排期（当前仅代码级保证）。

---

### 2.4 API 面

#### 2.4.1 REQ-03（C18）：盘选择语义

**问题**：06 的 C API 层 `single_plate` 从不赋值，`-p N` 被静默忽略实际全切
（实测 `-p 1` 切了 4 盘）。

**两步演进**：

| 步骤 | 内容 | 理由 |
|---|---|---|
| 1（95a2246） | 修复 C API `plate_id` → `single_plate` 赋值传递链 | 语义生效是前提 |
| 2（b739c20） | **CLI `-p` 整体移除**：传 `-p` 显式 rc=1 拒绝并指向 C API key | CLI 层冗余：盘选择是服务层关注点（per-plate 并行模式走 C API）；仓库内无 CLI `-p` 调用方（tests/scripts/tools/package/CI 全扫） |

**为何显式拒绝而非静默忽略**（对未知选项忽略策略的**记录在案的例外**）：忽略
`-p N` 会把单盘请求静默放大成全盘——N 倍工作量、不同产物名、不同退出码语义。
`-p -1 → rc=1` 验收继续成立（拒绝原因变为"选项已移除"）。

**验证**：C API `plate_id=1/2` 用例 + e2e Case 5/6/16 承接覆盖（无损失）；
`test_cli.cpp` deprecated 用例永久固化。

---

#### 2.4.2 JSON 统计增强（23d7e0c）

| 字段 | 语义 | 设计要点 |
|---|---|---|
| `support_free` | **gcode 实际层**（2026-10-10 二次换源，方案 C 落地）：`GCodeProcessorResult.print_statistics.support_volumes_per_extruder` 求和 >0（erSupportMaterial/Interface/Transition 三角色，普通/树/筏全覆盖），发射时取反（true=零支撑挤出）；两级 `print_info_total`/`plates[]` 同极性；成功时才发。配置开但自支撑几何 → true | **缺席 = 未知，坍缩在保守侧"有支撑"**；输出 gcode.3mf 内嵌盘元数据 `support_used` 保持配置层（桌面 PartPlate.cpp:5476 parity）。演进：同日先由 `support_required` 改名翻转（极性），再判定源配置层→gcode 实际层，均未发布前原地修订；顺带修 `PlateStats::support_used`（现 `support_generated`）失败盘早退留 garbage 值的初始化缺陷 |
| `mixed_filament_used` | 对象/part extruder 或面片选中虚拟混色槽（id 超过物理 `filament_colour` 数） | 载入时一次判定（`detect_mixed_filament_usage()`）——输入文件属性，部分失败运行仍有效；高度换色**刻意不查**（GUI 该处只支持物理耗材，用户确认） |

**验证**：`[capi][support]`×3 + `[capi][mixed]`×3；4 个 mixed fixture 覆盖
对象/modifier/面片三通路 + defined-but-unused 负对照。

---

### 2.5 交付（REQ-13 / C5）：生产 bundle 打包出厂自检

**问题**：bundle 打包从 fork 实时拷贝、无锁定；坏 bundle 进生产 → 所有请求在
预设装载阶段整单致命（P-09 无降级路径）。06 打包零校验。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 打包脚本产出前对**打进产物的 bundle** 内嵌 python3 自检：instantiated 耗材预设沿 inherits 链合并后必须解析出 `filament_vendor`；链不得悬空/成环；JSON 可解析；空 bundle 拒绝 | 检查的是产物而非源（防"源好拷坏"）；零新基建 | 自检逻辑自身需维护（判例注明红测构造法） | **采用**（e33d9f4） |
| B. CI 侧校验 | 集中管理 | 打包与校验分离，本地打包绕过；CI 基建依赖 | 否决 |
| C. 引擎运行时对坏 bundle 降级（非致命） | 容错 | 违背预设替换哲学（不可信配置流入切片） | 否决 |

**红绿矩阵**（实测）：健康 bundle PASS（312 预设/222 实例化/0 悬空/0 缺）；
删 instantiated 预设父级 → rc=1（4 悬空+4 缺 vendor）；坏 JSON → rc=1；空目录
→ rc=1。注意：删无 instantiated 继承者的文件（如 `fdm_filament_common`）不会红
——构造红测须删 instantiated 预设的直接父级。

---

## 三、未修项与候选方案（评审决策点）

### 3.1 P-08：擦塔位置 clamp 缺失 → 误报 TOOLPATH_OUTSIDE

**状态**：根因已定位，方案已定未实施（WIP 在 `stash@{0}`）。fixture
`2c242c10f0e4b521.3mf` 已入库。

**问题**：桌面 GUI 渲染塔预览时 clamp 进盘内并**回写** wipe_tower_x/y 到工程
配置；引擎无 GUI 盲信 3MF 值 → 塔顶超界 0.21mm → `TOOLPATH_OUTSIDE`（rc=7）
拦截本是正确行为，但合法文件被拒。

**候选方案**：

| 方案 | 优点 | 缺点 |
|---|---|---|
| A. `process_plate` 前算 depth + clamp + 回写 m_config（桌面移植） | parity 治本 | **直移植不够（已实测）**：clamp 的塔延展量不能只用 depth，需含 brim(5) + 塔底锥度（cone base）——参考 `Print::first_layer_wipe_tower_corners`（Print.cpp:2985-3005）做 bbox 级 clamp |
| B. 升级 libslic3r | — | **无效**：切片核心从不做此事（clamp 是 GUI 职责） |
| C. 用户侧绕行（已实测）：Orca「导入」方式重存（导入忽略塔位自动生成） | 零开发 | 运营负担；治标 |

**建议**：按 A 实施，验收标准 = clamp 后 wipe_tower_y=242.5 且无 TOOLPATH_OUTSIDE。
完整调研：`~/Downloads/擦塔越界调研报告-2c242c10f0e4b521.md`。

### 3.2 P-10：5479 ToolOrdering 堆损坏（上游）

**问题**：`fill_wipe_tower_partitions` raft-gap insert 分支 SIGSEGV；**桌面版
同模型同崩** → 上游 libslic3r bug。崩溃时 `lt_extra.print_z = 0xCCCCCCCD`
（乱码）→ 堆在进入函数前已被写坏，此处只是第一个撞上的分配。触发条件疑似
擦除塔 + 大首层间隙 + 树支撑 auto-lift 组合。

**已证伪修复（勿重走）**：`lt_object` 引用改值拷贝（无效）；insert 后刷新全部
back-pointer（改动确认进二进制，崩溃不变）。

**候选方案**：

| 方案 | 优点 | 缺点 |
|---|---|---|
| A. ASan 定位：上游 CMakeLists.txt:400 已有 `-fsanitize=address` 开关，建 `build-engine-asan` 独立目录重编跑 5479 | 定位真源头（当前两证伪修复都是盲修） | 全量 ASan 重编耗时；上游修复周期不可控 |
| B. 继续搁置 | 零成本 | 生产触发面存在（真实模型 5479 已撞上）；崩溃即整单失败 |

**建议**：维持搁置但排期 A（一次性投入定位，修复交上游）；上游源码留有两个
未提交防御性补丁（ToolOrdering.cpp，无害）。

### 3.3 C6：脏 `printer_settings_id`（`MyToolChanger 0.4 nozzle - Copy`）

**问题**：脏值烧在打包文件
`package/resources/profiles/Snapmaker/machine/Snapmaker U1 (0.4 nozzle).json:79`
（4 个 nozzle 变体都脏），随 wholesale 覆盖带入产物。06 与 2.2.0 行为一致（均现）。

| 方案 | 优点 | 缺点 |
|---|---|---|
| A. fork/bundle JSON 数据侧清洗（改回正确预设名） | 治本；桌面端同享修复 | 根因在 fork 仓库，跨团队协调 |
| B. 引擎侧覆盖后改名回官方名 | 引擎自治 | 治标掩汽数据；fork 侧持续再产脏值 |

**建议**：A，责任方在 fork/bundle 维护方；打包自检（REQ-13）未来可加 settings_id
规范校验防复发。

### 3.4 P-22 遗留：合成盘成员越床语义分歧

**问题**：合成盘成员越床走 `BUILD_VOLUME` 硬错误（rc=6）；桌面是"排除该实例 +
继续切其余"（GLCanvas3D `contained_min_one` 闸门）。`plateless_oob_fail.3mf`
（78bed5012）exit 6 属预期分歧，Case 23 锚定。

| 方案 | 优点 | 缺点 |
|---|---|---|
| A. 对齐桌面：越床实例排除 + warning + 切其余 | parity | **与既有"越界失败"e2e 语义冲突**（有盘文件越床=硬错的既有契约）；需"合成盘（或一般化）排除"单独设计评审 |
| B. 维持硬错 | 现状零改动；守门语义一致 | 合法无盘文件的床外实例拖死整单 |

**建议**：B 维持，A 列入 2.2.x backlog 待生产流量证明有真实文件卡在此处再启。

### 3.5 上游缺陷上报清单（fork/libslic3r，引擎侧已绕）

| # | 缺陷 | 引擎侧现状 | 上报动作 |
|---|---|---|---|
| U1 | `PrintConfigDef` 构造缺 `init_filament_option_keys()` → 全 fork `set_num_filaments` no-op（桌面靠 GUI 同名函数掩盖） | 自建 23 键表绕开（P-21）；上游修复后引擎 helper 幂等可收敛 | 待报 fork |
| U2 | BBS 真盘同对象多实例被 obj_inst_map 单键 map 静默丢条目（bbs_3mf.cpp:4473，crafted_dup 实验双实例只切 1 个） | P-23 enroll 递增 key 顺带绕开（仅无成员记录盘） | 待报 fork |
| U3 | `Slicing.cpp:184` layer_config_ranges 缺 layer_height 无 null 检查解引用 | 引擎侧 sanitize（2.1.7） | 待报 fork |

### 3.6 小项遗留

1. **日志路径发散**（P-17 遗留）：`slic3r_c_api.cpp`（~:230）为推导日志路径独立
   重算 `generate_output_path`——换名发生时两次调用时间戳不同，日志文件名与产物
   文件名不一致。方案：一次生成两处复用（低风险小改）。
2. **高流量咬合用例缺口**：缺 `filament_max_volumetric_speed > 66` 用例让喷嘴档位
   真实咬合（P-13 三层判定方法学已备）。
3. **`error_message` 不进 JSON**（P-19 遗留）：零产物时 JSON success=false 无原因
   字符串；P-23 Fix B 的 NO_PLATES_PRODUCED 已部分缓解。独立 schema 议题。
4. **空盘在 `plates[]` 无条目**：观测走全局 issues（带 plate_id）；若调用方要求
   plates[] 覆盖全部盘需再决策（受 P-19 分级语义约束）。
5. **安全遗留（会话记忆级，未固化判例）**：2026-09-20 对抗审查发现的 zip-slip
   新通道（0x7075 symlink 属性，含 PoC）与 S1 输出侧问题——建议评审会确认后
   补判例并评估解压 entry 路径规范化校验（拒绝 `../`/绝对路径/symlink 越界）。
   **注：此条依据会话记忆，仓库内无判例文档，评审时需现场补证。**

### 3.7 REQ-12 验证缺口

log sink 修复是代码级保证，**未做同进程多任务运行时验证**（需 C API 长驻 DLL
模式连续两次切片，断言无重复日志行/无句柄泄漏）。建议随 DLL 长驻场景联调一并补。

---

## 四、验证体系与回归防线

| 防线 | 内容 | 现状 |
|---|---|---|
| 红先行 | 全部修复先写红测（36e8daa / 7053443 / 8bfbda9），修复后转绿 | 已执行 |
| 双引擎差分 | 06 包 vs 2.2.0 同 fixture 双 CLI 对比 rc + JSON issues + 产物解包（17 主场景 + 7 二级项） | 12/13 REQ 红绿实证（唯一例外 REQ-12 代码级） |
| 全量套件 | `python3 tests/run_tests.py build-static`，JUnit 落盘 | 2026-10-08：integration **780 用例/0 失败/0 跳过**、unit 211/0 |
| 黄金基准 | 桌面版产物逐位对比（P-22：21.28g/首移动逐字节一致） | 关键 parity 修复必配 |
| 出厂门 | 打包脚本 bundle 自检红绿矩阵 | 上线（e33d9f4） |
| 判例库 | P-01..P-23，含已证伪方案防重走弯路 | 24 篇持续维护 |
| 产物预清理 | 输出路径预清洁 + `OUTPUT_PATH_RELOCATED` 告警锁（093c015） | 防 P-18 毒化链复发 |

**运维约束**（P-14）：编译必须 `-j 2`；切 Orca 分支必须重建 libslic3r.a（旧归档
混链 → all_regions() SIGSEGV）；切片前查 df（磁盘满 = 连片 GCODE_EXPORT_ERROR）。

---

## 五、评审决策清单（建议会上逐项拍板）

| # | 议题 | 建议 |
|---|---|---|
| 1 | P-08 擦塔 clamp 是否立项（方案 A，含 brim+cone base 延展量） | 建议立项（方案已定、fixture 已备、WIP 在 stash） |
| 2 | P-10 ASan 定位投入与上游修复推动 | 建议排期一次性定位；修复交上游 |
| 3 | C6 bundle 脏数据清洗责任方与时点 | fork 侧清洗 + 打包门加 settings_id 校验 |
| 4 | P-22 越床分歧（排除语义 vs 硬错） | 维持硬错，backlog 待生产流量证明 |
| 5 | 上游上报 U1/U2/U3 排期与跟进 owner | 建议指定 owner |
| 6 | zip-slip/S1 安全遗留（会话记忆级） | 会上补证 → 补判例 → 评估解压校验 |
| 7 | REQ-12 DLL 长驻验证 | 随长驻场景联调补 |
| 8 | 2.2.0 发布基线确认（MR !3，含全部 13 REQ + P-22/P-23 + 780 用例绿） | 建议合并发布 |

---

## 附：文档索引

- 需求与验收：`docs/requirements/fix-requirements-2.2.0.md`、`exit-codes.md`、`json-output.md`
- 差分实测：`REVIEW-DIFF-020106-VS-220-2026-09-28.md`（仓库根）
- 对抗审查：`REVIEW-C4-C5-2026-09-20.md`（仓库根）
- 判例库：`docs/precedents/`（README.md 为索引）
- 宪章与方法论：`CHARTER.md`、`METHODOLOGY.md`
