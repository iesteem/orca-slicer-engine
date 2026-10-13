# 云切片引擎测试体系技术方案（评审稿）

- **评审日期**：2026-10-12
- **版本**：feature-2.2.0 @ c4da825（Orca 2.4.1 base 对齐后首绿）
- **全量基线**：212 用例 / 0 失败 —— engine-tests 74 用例（<0.1s）+ engine-integration 138 用例（≈1433s）
- **资产规模**：24 个测试源文件；127 个 in-repo fixture（120MB，最大单件 11MB）
- **支撑材料**：`docs/precedents/`（判例 P-01..P-25 + 分层协议 + 环境约束）、`tests/fixtures/MANIFEST.md`、`docs/requirements/`（exit-codes / json-output 契约）、`tests/run_tests.py`

> 本文分两部分：§1–§4 是**现状与实证**（全部断言带 commit / file 出处，可直接核验）；
> §5 是**演进提案**（候选方案对比，未实施项均标注"提案"，不与已验证事实混写）。

---

## 一、总体设计原则（贯穿全部选型）

| # | 原则 | 出处 |
|---|---|---|
| T1 | **行为锚点优先**：断言"输入 → 可观察输出契约"（exit code / JSON 字段 / 产物存在性 / issue code），不断言内部结构与函数返回 | test-layering.md |
| T2 | **判例高于测试**：判例可推翻测试，测试不可悄悄偏离判例；anchor 断言与桌面实测冲突时以 `docs/precedents/` 为准 | test-layering.md 验收协议 §2 |
| T3 | **fixture 是被继承的数据**：生产原件保留哈希名禁重命名 + md5 完整性校验；自制 fixture 语义化命名可按配方再生 | fixtures/MANIFEST.md |
| T4 | **红跑可诊断、不必重跑**：每 case 的 JUnit XML 落盘，失败从报告定位，不重跑 24 分钟套件 | run_tests.py（45cc024） |
| T5 | **漂移先钉死、再谈对错**：base 同步引起的期望变化，先 CLI 复现确认现状，翻转断言钉死并注释留上游 commit 链，检测口转移要显式声明 | c4da825 |
| T6 | **红测先行**：修 bug 先写会红的用例（`test(red):` 提交），修复提交使其转绿；禁止"顺手全绿"的补充测试 | 36e8daa、7053443、8fbf9ad |
| T7 | **宪法级约束进测试**：all-or-nothing 打包、`post_process` 恒空（RCE 面）这类不可退让的决策直接用 anchor 用例钉死，防止后续无感漂移 | P-05、test_gcode_security.cpp |

---

## 二、体系现状

### 2.1 双目标结构：亚秒级单元层 + 全量集成层

| 目标 | 链接 | 内容 | 耗时 |
|---|---|---|---|
| `engine-tests` | **不链 libslic3r**（gc-sections 手法） | Utils / Types / PlateGrid / SlicingDeadline / BuildVolumeClassify / ValidateClassify 纯 helper | 74 用例，<0.1s |
| `engine-integration` | 全量 libslic3r（ORCA_STATIC_LIBS） | 直接 C++ 驱动 `SliceEngine::run()` 全流程 + 真实 CLI 子进程 | 138 用例，≈24min |

**关键设计决策**：

- **单元层脱离 libslic3r 的手法**（tests/CMakeLists.txt:33-44）：`Utils.cpp` 中
  `resize_thumbnail()` / `format_exception_context()` 引用 libslic3r 符号，测试只测
  纯函数。选型是 `-ffunction-sections -fdata-sections -Wl,--gc-sections` 链接期剔除
  未引用函数，而不是把 Utils.cpp 拆文件——保持生产源码单一事实，代价是构建标志
  特殊化。Boost 头与静态归档复用 `ORCA_STATIC_INCLUDES` / `ORCA_STATIC_BOOST_LIBS`
  （StaticConfig.cmake 捕获），测试目标零硬编码路径。
- **集成层镜像主目标源列表**（tests/CMakeLists.txt:84-97）：与 orca-slice-engine
  目标同源（减 main.c），测试与生产二进制同编译单元，杜绝"测试链的代码不是线上
  代码"。`test_cli.cpp` 走真实子进程（`ORCA_ENGINE_EXE` 生成器表达式注入 +
  `add_dependencies`），覆盖 C API 之外的 argv 入口。
- **编译期注入契约**（tests/CMakeLists.txt:134-142）：fixture 路径、资源根
  （`ORCA_TEST_RESOURCES_ROOT` = package/resources，C API 自拼 `/profiles`）全部
  编译定义注入，测试源内零绝对路径（2faab6e 修正过"资源根传到 profiles 子目录"
  的契约错位，已固化）。

### 2.2 分层协议：[anchor] / [impl] / mixed

`docs/precedents/test-layering.md` 按断言对象把 24 个测试文件三分：

- **[anchor]**（16 个文件）：断言输入→输出契约。代表：test_slice_e2e（exit code +
  JSON 产物契约）、test_gcode_security（`post_process` 恒空硬契约）、test_boundary
  （喷嘴/层高/温度越界契约）。
- **[impl]**（test_types / test_utils / 内部辅助）：断言内部函数，随实现废弃。
- **mixed**（5 个文件）：契约部分是锚点、访问器部分是实现（如 test_geometry_preprocess
  的 Z-bake 不变量 vs `model()` 访问器）。

**转生验收协议**（重构时启用）：新实现必须在全部 anchor 用例上通过且 fixture 不变；
impl 用例失效不阻塞重构，但其覆盖的内部逻辑变更须记 CHANGELOG。这份协议是测试
体系对"下一代实现（含 AI 重写）"的接口承诺——anchor 层即引擎的行为规格。

### 2.3 Fixture 治理

- **命名律**（MANIFEST）：生产原件保哈希名（判例文档与事故记录的引用锚点，禁改名）；
  自制/派生 fixture 语义命名，名字即行为说明。
- **in-repo 决策**（2026-09-09）：历史外部池（`~/Downloads/切片引擎测试-3MF文件包`）
  已废弃，全部 fixture 进仓库（现状 127 件 / 120MB）。理由：外部池依赖单机存活，
  判例 fixture 丢失的教训已经发生 3 次（P-01/P-02/P-10 原件已不在本机，根因仅存文档）。
- **判例锚定**：判例 fixture 登记 md5 + 锚定行为，例如 `2c242c10f0e4b521.3mf`
  （P-08，擦塔超界 0.21mm → exit 7）标注"未修复，修复后应转绿"——fixture 同时是
  未修复缺陷的存在性证明与修复验收器。
- **产物断言纪律**：断言输出产物的用例必须预清输出目录——`generate_output_path()`
  在路径被占用时静默换名（af80f0b 已加 OUTPUT_PATH_RELOCATED 告警，但测试不能依赖
  告警），093c015 已全量预清。

### 2.4 运行与报告链

```
cmake --build → ctest -R engine（2 个 test entry）
                ├─ console reporter（--output-on-failure 兼容）
                └─ junit::out=<build>/tests/*.junit.xml（每 case 名 + 状态 + 耗时）
python3 tests/run_tests.py <build>        # 包装 ctest + 解析全部报告 + 失败摘要
python3 tests/run_tests.py --report-only # 红跑后从报告诊断，不重跑
```

失败摘要截取每 case 前 8 行 / 240 字符（位置 + 展开式，长日志噪音裁掉）。
**报告采信协议**（踩坑固化）：SIGTERM 中断产生的报告是合法 XML 但只含已执行用例——
采信前必须 grep 新增用例名 + 比对总数（当前全量 212：integration 138 / unit 74）；
`run_tests.py` 不做构建，先 `cmake --build` 再跑。

### 2.5 Tag 分类与覆盖矩阵

集成层 19 个域 tag（每用例 `[integration][域]` 双 tag），用例分布实测：

| 域 | 用例 | 域 | 用例 | 域 | 用例 |
|---|---|---|---|---|---|
| slice（e2e 主流程） | 37 | tempmode | 17 | capi | 17 |
| layerheight | 9 | cli | 9 | pathconflict | 7 |
| geom | 7 | fileval | 7 | filcount | 6 |
| printer/preset/gcodesec/filament | 各 5 | seq/boundary/accuracy | 各 4 | unsupported | 3 |
| highflow | 2 | brim | 1 | | |

run() 主流程覆盖矩阵（7 个 [slice] 场景类 + 补齐缺口）维护在团队记忆与
e2e 源注释中：成功/越界失败/超高失败/部分沉床/空盘跳过/全空盘失败/打包抑制/
无盘号请求/plate-less 合成盘。

### 2.6 知识库联动（测试不是孤岛）

- **判例库**（`docs/precedents/`，P-01..P-25）：每条已实测行为契约 + 根因 +
  已证伪推测。测试断言与判例冲突时判例胜（T2）；修 bug 的验收 = 判例从"未修复"
  翻转为"已修复" + 对应用例转绿。
- **契约文档**（`docs/requirements/exit-codes.md`、`json-output.md`）：exit code
  表与 JSON 字段表是 anchor 断言的规格源，28ae3c3 起与 2.2.0 代码同步。
- **需求文档**（`fix-requirements-2.2.0.md`，13 REQ）：每 REQ 落地为红→绿用例组，
  回归审查（2026-10-10）按 22 变更项逐项补锚后全绿。

---

## 三、2.4.1 对齐实战：三类漂移与既成协议

2026-10-10 合入上游 b626b4be62（Orca 2.4.1 base sync）后，c4da825（2026-10-12）
处理了三类期望漂移。这是"base-bump 漂移"的完整实证样本，演进方案 §5.2 以此为输入：

| 类型 | 实例 | 处置 |
|---|---|---|
| **A. 配置面漂移**（新键族） | `top/bottom_color_penetration_layers` 是新 PrintRegionConfig 键；旧项目不携带，替换契约不加键（`overwrite_all_keys_from` 只覆写已有项）→ 代码默认 4 生效 vs 官方预设显式 5 | 键族豁免进 `proc_skip`，**按族不按键**豁免——未来项目携带键的真实分叉仍会红；注释留完整根因链与桌面 parity 论证 |
| **B. 行为反转**（fixture 条件消失） | Case 9 擦塔 fixture：上游重做擦塔分区后塔足迹缩回床内，越界条件不再复现 | 翻转断言钉死新现实（exit 0 + 产物存在）；**检测口转移显式声明**：TOOLPATH_OUTSIDE 检测端口覆盖由 Case 11（toolpath_outside_mixed_7855）承接，不留孤悬 |
| **C. 定罪组成变化**（多盘混合） | Case 12.5 四盘混合：盘 1 的 TOOLPATH_OUTSIDE 随 B 类根因消失，仅剩盘 3 的 BUILD_VOLUME_OUTSIDE_XY | 保留真正不变的核心断言：逐盘定罪、干净兄弟盘隔离、退出码类别聚合（exit 6） |

**既成协议（T5 的操作化）**：漂移发现 → CLI 单独复现确认非测试环境噪音 → 钉死
当前行为（翻转/收紧断言）→ 注释留上游 commit 哈希与根因 → 声明检测口归属 →
提交信息记录"全量 N 用例 / 0 失败"作为基线快照。

---

## 四、已知盲区（诚实清单）

以下是当前体系**明确知道未覆盖**的部分，按风险排序：

| # | 盲区 | 风险 | 现有止血 |
|---|---|---|---|
| G1 | **brim 失败路径无锚**：2.2.0 回归审查 22 项中唯一开放盲区（③），brim 相关失败场景（回显恢复失败、I/O 异常传播）无用例 | 中：P-11/P-20 家族回归不可测 | P-20 stage 2.1 修复时 f291619 对抗审查过，判例含遗留 P3 清单 |
| G2 | **上游搁置 bug 无守护**：P-10（5479 堆损坏，源在更早路径，两修复证伪）搁置；P-08（擦塔 clamp 0.21mm）未修复 | 中：崩溃家族可能随 base 同步复发/变化 | P-08 fixture 钉死未修复行为（修复后应转绿）；P-10 判例含 ASan 入口指引 |
| G3 | **无覆盖率度量**：从未跑过 gcov/lcov，盲区发现靠审查与事故驱动 | 中：新增代码无锚不自知 | 判例 + 回归审查人工补锚（2026-10-10 补齐 shrink 侧与合法边界） |
| G4 | **无性能基准**：junit 已记录每 case 耗时但无人比对趋势；base 同步引入切片性能退化不可见 | 中：生产切片时长劣化无感 | 无 |
| G5 | **无 CI**：红绿依赖本机手动（-j2 资源约束、构建产物治理靠纪律）；build-shared 曾有陈旧二进制陷阱（v02.01.06 不含修复） | 高（流程性）：回归发现时点 = 下次有人想起来跑测试 | run_tests.py 一键化；junit 落盘 |
| G6 | **大压力 fixture 缺位**：星空猫级多盘大模型（峰值 ~2.5G 磁盘）未入库，20 盘规模路径只在生产踩过 | 低-中：规模化 bug（磁盘满连锁 GCODE_EXPORT_ERROR）无回归锚 | 判例 disk-space 运维记录 |
| G7 | **3 个判例原件永久丢失**（P-01/P-02/P-10）：判例文档存根因但无法再造 fixture | 低（已修复缺陷） | 判例文档完整留档 |

---

## 五、演进方案（提案）

> 本节为提案，未实施。每个方向按 问题 → 候选对比（含否决）→ 选型 → 验证 给出。

### 5.1 CI / 流水线化（对应 G5）

**问题**：测试价值 = 覆盖 × 执行频率。当前覆盖 212 用例，执行频率 = 人工自觉。
构建重（静态链 OCCT/CGAL/Boost/OpenCV/TBB，Boost 私有 deps 树）+ 本机 -j2 硬约束
（P-14），决定了"本机随手全量"不可持续。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 托管 CI（GitHub Actions / GitLab CI，按仓库托管方）+ Docker 镜像固化构建环境 | PR 门禁自动化；环境可再生（根治 build-shared 类陈旧产物）；-j2 约束随宿主消失 | 镜像首次构建重（deps 树大）；需托管方 runner 资源 | **目标态** |
| B. 本机定时全量（cron + run_tests.py + junit 归档目录） | 零新基础设施；当天可落地 | 仍占用工作机；门禁时点 = 发现时点，不在合并前 | **阶段 0 立即做** |
| C. 只跑单元层作为日常门禁（<0.1s pre-push hook） | 极速反馈 | 138 个集成用例（全部行为锚点）被排除在门禁外，防护面骤减 | 仅作为 A 的组成部分，不单独存在 |
| D. 集成层并行拆分以适配 CI 时限 | 墙钟缩短 N 倍 | Catch2 无内建并行：需按 tag 拆进程 + 合并 junit；fixture 产物目录需隔离防互踩 | 作为 A 的配套（见 5.4） |

**选型（分阶段）**：

1. **M0（本周，零代码风险）**：cron 每夜全量 → junit 归档到带日期目录 →
   `run_tests.py --report-only` 输出摘要。归档目录即"最近一次绿"的存在性证明。
2. **M1**：Docker 镜像固化构建环境（base 镜像装编译器 + CMake；deps 树与
   StaticConfig 产物 COPY 进镜像；`build-environment-constraints.md` 的 libslic3r.a
   重建陷阱以镜像层缓存规避）→ 托管 CI 上 PR 门禁 = 单元全量 + 集成冒烟子集，
   夜间 = 集成全量。
3. **冒烟子集定义（提案）**：`[boundary]` + `[fileval]` + `[printer]` + `[slice]`
   的失败路径用例 + `[gcodesec]` 全部 ≈ 30 用例（Catch2 tag 表达式过滤），
   目标 <8 分钟。子集选取标准：宪法级约束（T7）+ 高频回归域 + 失败代价高域。

**验证**：门禁红绿演练——人为注入一个会红的断言（临时分支），确认 CI 拦截；
再确认 junit 归档完整含 212 用例（防 §2.4 截断陷阱在 CI 复发）。

### 5.2 base-bump 漂移治理（T5 的工具化）

**问题**：§3 的协议目前是口传 + 提交信息。下一次 base 同步（2.4.x → 更新）时，
"哪些红是漂移、哪些是回归、哪些检测口需要转移"仍要人工重新发明流程。

**候选方案**：

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. checklist 文档（`docs/process/base-bump-checklist.md`）：CLI 复现 → 分类 A/B/C → 钉死 → 检测口声明 → 基线快照 | 零工具成本；协议从口传变文档 | 仍依赖人工执行 | **M0 立即做** |
| B. 期望快照 diff 工具（`scripts/diff_expectations.py`）：全量跑出 per-fixture 摘要（exit code + issue codes + 产物存在性），base 前后两份 JSON diff，自动列出漂移候选 | 漂移候选自动穷举（人工只处理列出的项，不会漏）；产物即对齐报告 | 需要全量跑两遍（base 前后各一次 ≈ 2×24min） | **M2 做** |
| C. 黄金文件全量产物比对（gcode 字节级 golden） | 最严格 | 过脆：gcode 内容随 base 合法变动（时间戳、路径排序），红噪音淹没真信号 | **否决**（半严格变体——比对 gcode 行数 ± 容差——可作 B 的字段扩展） |

**选型**：A 先行固化协议；B 落地为工具。B 的输出直接对接 checklist 的"分类"步骤：
diff 列出的每一项必须被归入 A/B/C 类或判为回归，处理结论回写对齐报告。

**验证**：以 2.4.1 对齐重演——在 2.2.0-base 与 2.4.1-base 各跑一遍，工具应列出
c4da825 已知的全部 3 处漂移（color_penetration 键族 / Case 9 / Case 12.5），
不多不少为通过标准。

### 5.3 覆盖率与盲区收敛（对应 G1/G2/G3/G7）

**问题**：盲区发现是事故驱动；覆盖率从未量化；两个已知未修复缺陷没有守护性测试。

**候选方案与选型**：

1. **覆盖率度量（G3）**：
   - 范围只算引擎自有代码（`src/*.cpp`：SliceEngine / slic3r_c_api / PresetRollback /
     JsonReport / GeometryCheck / Utils）——libslic3r 是上游，度量它只会产出无法
     行动的数字。
   - 手法：`--coverage` 编译一个测试专用 build 目录（不动生产 build-static），
     lcov 出报告；先取基线，不设阈值门禁（第一步是让"哪里没锚"可见，不是罚分）。
   - 风险：集成层 gcov 数据合并 24 分钟跑一遍的开销——放进夜间全量即可。
2. **G1 brim 失败路径（红测先行，T6）**：按 P-20 判例的遗留 P3 清单逐条写
   `[brim]` 失败路径用例（当前该 tag 仅 1 用例）。预期先红后绿或钉死现状，
   两者都是收敛。
3. **G2 搁置 bug 守护**：
   - P-08：fixture 已在库（2c242c10），当前钉的是未修复行为——保持"修复后转绿"
     契约即可，无需新增。
   - P-10：按判例 ASan 入口指引做一次性 ASan 全量跑（本机 -j2 下耗时可观，可与
     5.1 M1 的 CI 环境合并做）；产出要么定位根因要么把"ASan 干净"记录为基线事实。
4. **G6 大压力 fixture**：三选一——全入库（仓库已 120MB，星空猫级会显著膨胀）/
   git-lfs / **派生缩减版入库 + 原件外部引用**（保留 20 盘拓扑但压缩几何细节）。
   倾向派生缩减版：锚定的是规模路径（盘数、磁盘、打包）不是几何本身。
   此项需人类决策（涉及仓库策略）。
5. **G7 判例原件丢失**：不可逆，仅维持判例文档完整性，无行动项。

**验证**：覆盖率出基线报告；`[brim]` 用例数 1 → ≥4；ASan 一次全量有结论落盘。

### 5.4 性能与耗时基准（对应 G4 + 套件提速）

**问题**：两层——套件本身的 24 分钟墙钟（CI 化的阻力），与被测物的切片性能
（base 同步 / 引擎改动可能引入劣化，当前不可见）。

**候选方案**：

| 子问题 | 候选 | 结论 |
|---|---|---|
| 套件墙钟 | a) 按 tag 拆 N 个并行进程（各自 junit，`run_tests.py` 已支持多报告合并摘要） | **采用**（与 5.1 D 配套）。前提：每进程独立输出目录（产物互踩是已知陷阱，§2.3） |
| | b) Catch2 换支持并行的框架 | 否决：212 用例 + anchor 断言语义迁移成本高，且 test-layering 已声明"允许迁移框架但断言语义不可变"——留到转生时再议 |
| | c) 切片结果缓存（同 fixture 跳过） | 否决：切片输入 = fixture + 预设 + 引擎二进制，三者任一变化缓存即失效，命中场景仅剩纯重构——而重构恰恰要真跑 |
| 切片性能回归 | a) junit per-case `time` 属性趋势比对脚本（阈值告警，如单用例同比 ±30%） | **采用（低成本先行）**：数据已在落盘，只差一个 diff 脚本。噪声控制：仅夜间全量（本机空闲窗口）比对，连续两晚超阈才告警 |
| | b) 固定压力模型专项基准（星空猫级， nightly 长跑 + 切片前 df 预检） | **采用（依赖 5.3-G4 fixture 决策）**：磁盘满是已知连锁崩溃源（disk-space 判例），基准同时是规模回归锚 |

**验证**：并行拆分后合并报告总数仍 = 212（防拆分丢用例）；趋势脚本对一次已知
性能变化的 base 同步（可拿 2.4.1 对齐前后两份 junit 实测回放）能正确报警。

---

## 六、里程碑与验收

| 里程碑 | 内容 | 验收标准 | 状态 |
|---|---|---|---|
| M0 | base-bump checklist 文档 + 本机 cron 夜间全量 + junit 归档 | 归档目录出现连续基线快照；checklist 评审通过 | **已落地（2026-10-12）**：`docs/process/base-bump-checklist.md` + `scripts/nightly_tests.sh`（flock 防重入、3GiB 磁盘预检、14 天 rotation）+ crontab 02:17；端到端验证 ALL GREEN 212 用例（1401s），归档含 counts.txt 守卫（74+138） |
| M1 | Docker 构建环境 + 托管 CI（PR 冒烟门禁 + 夜间全量） | 红绿演练通过；冒烟 <8min；夜间全量 212 用例归档 | 提案 |
| M2 | 期望快照 diff 工具 + junit 耗时趋势比对 | 2.4.1 对齐重演列出全部 3 处已知漂移；趋势脚本对已知变化正确报警 | 提案 |
| M3 | 覆盖率基线 + `[brim]` 失败路径锚 + ASan 全量结论 | 覆盖率报告落盘；brim 用例 ≥4；ASan 有结论（定位或记录基线干净） | 提案 |

**不做清单**（明确出界，防止范围蔓延）：

- 不给 libslic3r 上游代码写测试（上游 bug 走判例 + 上报，D6/T2 原则）。
- 不做 gcode 字节级黄金文件比对（已否决，§5.2-C）。
- 不在本期换测试框架（转生时按 test-layering 协议处理）。
- 不设覆盖率百分比门禁（先可见后治理）。

---

## 附：一页速查

```bash
# 构建 + 全量（本机必须 -j 2）
cmake --build build-static --target engine-tests engine-integration -j 2
python3 tests/run_tests.py build-static            # ctest + 失败摘要
python3 tests/run_tests.py --report-only build-static   # 红跑后读报告
# 采信报告前：grep 新用例名 + 比对总数（212 = 74 unit + 138 integration）
```
