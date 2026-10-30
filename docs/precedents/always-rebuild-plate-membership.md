# P-24: 非空 plate membership 与几何错位 → 3 盘误判越界零产物（always-rebuild）

**状态：** 已修复（feature-2.2.0，2026-10-10）
**Fixture：** `tests/fixtures/2d986d79661ffb62.3mf`（md5
`0794e9323efb3fe073c603c6b493c951`，生产原件入库）；e2e Case 28、
Case 15 新增 tip 静默锁
**推翻关系：** 经人类批准（2026-10-10）推翻 P-23 的"仅空 map 盘候选"
收敛决策；P-23 Fix A 的重建机制本体被本判例完整继承并扩展，Fix B
（NO_PLATES_PRODUCED）不受影响。

## 现象

生产文件 `2d986d79661ffb62.3mf`（8 盘，第三方导出器产出——自定义工艺
预设名 "Litos"、260mm 热床网格排版为证）：

- 引擎判盘 3/5/8 `BUILD_VOLUME_FULLY_OUTSIDE`（top.stl / side.stl /
  orangeQuestion.stl"完全在打印体积外"）→ rc=6；all-or-nothing 打包
  （[[packaging-all-or-nothing]]）连带 4 个成功盘也零产物
- **桌面 Orca 2.4.0 对同一文件全部 8 盘切片成功**（用户提供输出
  `marcoLamparaSuperior_PLA_4h13m.gcode.3mf` 为证），且桌面输出的盘
  归属与文件元数据**不一致**：top.stl 在盘 2（元数据记盘 3）、
  side.stl 在盘 3（记盘 5）、orangeQuestion.stl 在盘 5（记盘 8）

## 根因

文件内部 membership 元数据与几何格位错位一格：8 个对象全部精确落在
"7 盘布局"的格位中心（网格间距 312mm = 260 床 × 1.2），但
`model_settings.config` 的 plater_id 归属被第三方工具重排过、对象未随
之移动。引擎按当前 U1 打印机（270 床，间距 324mm）重算盘原点后，
错位对象的盘局部坐标落在体积外（top.stl → (-206,130) 等，逐一推演
与引擎成败 8/8 吻合）。

**引擎与桌面的分歧点**（P-23 Fix A 的"documented divergence"）：
桌面 `load_from_3mf_structure`（GUI/PartPlate.cpp:~5518）把从文件恢复
obj_inst_map 的代码**注释废弃**（"just test for file correct or not, we
will rebuild later"），结尾 `reload_all_objects()` **每次载入全量几何
重建归属**——membership 元数据从不被信任。P-23 Fix A 只把 obj_inst_map
**为空**的盘作为重建候选、非空 map 逐字信任，并自称"真实可达文件上
等价（有成员却漏记自己盘上实例的文件本就是损坏输入）"——本文件正是
该"损坏输入"的真实可达反例：membership 非空但错位，桌面能切、引擎
误判。

**桌面禁切语义（实证，决定告警级别）**：GLCanvas3D.cpp:3973 切片按钮
enable 条件 = `contained_min_one && !partlyOut`——**完全出界不阻断**
（实例落 unprintable plate，其余盘照切；fullyOut 的提示代码被 BBS 注释
关闭），仅部分越界禁切（引擎已有 BUILD_VOLUME_OUTSIDE_* 阻断路径）。

## 修复

`SliceEngine::rebuild_plate_membership()` 从"仅空 map 盘候选"扩为
**always-rebuild**（对齐桌面 reload_all_objects 全量语义）：

- **P-22 守卫（新增）**：`m_synthesized_single_plate` 时直接返回——
  合成单盘的"盘 1 = 全部 printable 实例"是位置无关判例语义，几何重建
  会把床外实例剔出（静默丢件 / 误触 PLATELESS_NO_PRINTABLE_INSTANCES）
- **按实例身份快照 before**：`loaded_id → (obj_idx, inst_idx)` 索引 +
  仅 printable 且可解析的记录计入。必须按身份而非 identify_id 值比对
  ——重建后 `loaded_id = id().id` 与文件 identify_id 数值必然不同
- **清空所有盘 map → 全盘盒几何重算**：凸包 bbox × eps 膨胀盘盒
  （`orca::compute_plate_box` 沿用），盘序首个命中胜；enroll 沿用
  P-22 模式（loaded_id + 递增 key 成对写入，6 个匹配点零改动）
- **不落任何盘的 printable 实例** → warning 级
  `INSTANCE_NOT_ON_ANY_PLATE`（非阻断，对齐桌面"完全出界不阻断"实证）
- **tip 条件重设计**：从"enrolled > 0"改为"任一 printable 实例归属
  变化（before vs after 集合差异）"——桌面-authored 文件
  geometry ≡ membership，重建为 no-op 且**静默**（Case 15 新增
  REQUIRE_FALSE(tip) 锁定）；Case 17（printable=0 双空）保持静默；
  Case 26/27（无记录 → 有归属）保持发 tip

### 已知局限

- 同一实例被多个盘的 map 重复声明（损坏输入，导入器 loaded_id 后写
  胜出）：before 快照按盘收集会把同一身份收进多盘集合，归属差异仍能
  检出，但"具体从哪盘搬到哪盘"的计数可能不精确——仅损 tip 文案精度，
  不损归属正确性
- 上游 bbs_3mf.cpp:4473 单键 map 丢同对象多实例条目的缺陷在
  always-rebuild 下被全面绕开（不再有"信任非空 map"路径依赖它），
  属顺带 parity 改善

## 验证证据

- 事故件手测（本机 build-static，2026-10-10）：
  - 修复前：rc=6、盘 3/5/8 BUILD_VOLUME_FULLY_OUTSIDE、零产物
  - 修复后：rc=0、**8 盘全部 success**、PLATE_MEMBERSHIP_REBUILT tip、
    产物 gcode.3mf 35MB、逐盘耗材/时间非零
- **桌面对照 1:1**：产物 gcode 的 EXCLUDE_OBJECT_DEFINE 逐盘对象名
  = 桌面输出 plate_1..8 对象序列（black / top / side /
  marcoLamparaAjustado / orangeQuestion / marcoLampara / 
  marcoLamparaInferior / marcoLamparaSuperior），对象 CENTER 全部落床内
- e2e：Case 28（事故件 8 盘全绿 + tip + 无 INSTANCE_NOT_ON_ANY_PLATE +
  无 BUILD_VOLUME_FULLY_OUTSIDE）；Case 15 加 tip 静默锁（显式 map 与
  几何一致的文件不得产生重建噪音）
- 网格外实例正负两向锁（2026-10-10 补，构造件由用户桌面制备）：
  Case 29（slice_offgrid_sibling_ok.3mf：网格外实例 + 格内好盘 →
  rc=0、好盘出产物、INSTANCE_NOT_ON_ANY_PLATE warning + EMPTY_PLATE +
  tip 三者并存，非阻断契约）；Case 30（slice_offgrid_sole_fail.3mf：
  唯一实例网格外 → rc=6 源自 NO_PLATES_PRODUCED 零产物守卫而非警告，
  失败归因钉死，警告不得悄悄硬化为阻断）
- 定向 [membership] 5 用例绿；全量套件见提交时 junit 记录
  （`python3 tests/run_tests.py build-static`）

## 已证伪 / 否决的方案

- **validate-then-rebuild（分歧才重建）**：被否决——代码更多、引入
  "检测分歧"的新语义边界，而桌面根本不做检测（无条件重建）；
  always-rebuild 在桌面-authored 文件上结果与元数据相同，行为面等价
- **床外实例保持 P-23 前的阻断 error**：被否决——桌面实证完全出界
  不禁切（unprintable plate 语义），阻断会造成"桌面能切引擎不能切"
  的反向 parity 缺陷；warning 兼顾云侧可观测性
- **顺带把 P-22 合成单盘也纳入几何重建**：被否决——合成盘语义
  （位置无关 enroll-all）是 P-22 判例核心，纳入重建等于推翻 P-22，
  超出本判例范围且无生产反例支撑

## 判例推翻记录

P-23"已证伪 / 否决的方案"一节曾否决"桌面全量几何重建"，理由是
"回归面不可控"。2026-10-10 生产反例（本判例现象）证明该收敛在真实
文件上失败；用户（人类）于 2026-10-10 明确批准采用 always-rebuild，
满足 `docs/precedents/README.md`"判例只能被人类明确批准后才能推翻"
规则。P-23 文档已加状态修订注。
