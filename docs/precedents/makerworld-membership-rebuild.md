# P-23: 盘元数据无成员关系 → 假空盘零产物失败（JSON 无 error 级可见）

**状态：** 已修复（feature-2.2.0，2026-10-08）
> **2026-10-10 修订（P-24）：** Fix A 的"仅空 map 盘候选"收敛已被生产反例
> （membership 非空但与几何错位一格，桌面全切引擎误判 3 盘）证伪，经人类批准
> 扩为 always-rebuild（见 always-rebuild-plate-membership.md）。本判例的重建
> 机制、P-22 enroll 模式、Fix B 与全部 fixture 继续有效；"已证伪/否决的方案"
> 一节中否决"桌面全量几何重建"的决策同时作废。
**Fixture：** `tests/fixtures/1e9d9919e7955ce4.3mf`（md5
`70cd1bcba83e12d21477ee074b01b0f0`，生产原件入库）、
`slice_membership_rebuilt_2plate.3mf`（派生，md5
`918f7b5a34631be912685e522545bb57`）、
`slice_all_plates_empty_fail.3mf`（重生成，md5
`22b628d983847e41fe5aef2e7e602f8d`）；e2e Case 26/27、Case 17 更新

## 现象

生产文件 `1e9d9919e7955ce4.3mf`（MakerWorld 下载特征：DesignModelId/
DesignProfileId；`BambuStudio-02.06.00.51` / `OrcaSlicer 2.4.2` 写出）：

- JSON issues 共 123 条**全部 warning**（0 条 serious_warning / error），
  但 `success=false`、`plates: []`、exit 6
  「No plates completed successfully」——只读 issue 列表的调用方会把一次
  硬失败误判为成功
- 日志链：`Marked 0 instances printable` → `Skipping empty plate` →
  `No plates completed successfully`
- 桌面版 Orca 2.4.0（fork 同源）对同一文件切片成功；文件自身内嵌切片
  结果（slice_info：prediction 4997、weight 21.04、3 色耗材用量）证明
  作者机器确实切成功过

## 根因

新格式导出只写盘壳、不写成员关系，而引擎把 obj_inst_map 当唯一权威：

1. 引擎盘成员唯一来源 = `model_settings.config` `<plate>` 块内
   `<model_instance>` 子元素（fork bbs_3mf.cpp:4473 填
   `PlateData::obj_inst_map`；:2262 写 `inst->loaded_id`）。该文件 plate
   块只有 plater_id/缩略图等元数据，**0 个 `<model_instance>`**；实例
   本身存在（`3dmodel.model` 的 `<build><item printable="1">`，摆放信息
   在新式 `<assemble>` 节）
2. `mark_printable_instances`（SliceEngine.cpp）按
   `loaded_id ∈ obj_inst_map 导出集` 判定 → 空集 → 0 可打印实例
3. P-22 无盘合成兜底守卫是 `m_plate_data.empty()`——本文件**有**盘元数据
   （成员缺失），兜底不触发
4. 走 P-19 分支：`EMPTY_PLATE` **warning** + 跳盘（多盘语义下一个空盘
   不得拖死其余盘）；唯一盘被跳 → 零产物 → `finalise_statistics` 只写
   `error_message` 字符串 + 退出码，**不 push 任何 issue**——该分支是
   全库唯一"写 error_message 却不落 issue"的失败路径

**桌面端语义（实证）**：`PartPlateList::load_from_3mf_structure`
（GUI/PartPlate.cpp:5496）里 obj_inst_map 恢复代码**已被注释废弃**
（"we will rebuild later"），随后 `reload_all_objects()`（:4550）**几何
重算归属**：`instance_convex_hull_bounding_box` ∩ 盘矩形（SceneEpsilon
膨胀），首个命中盘获胜；不落任何盘 → unprintable plate。桌面根本不
依赖文件里的成员记录。

## 修复

**Fix A（功能，桌面语义移植）**：`SliceEngine::rebuild_plate_membership()`
（run() 内、apply_preset_substitution 之后 printable_area 终值就绪时、
逐盘循环前）：

- 候选盘 = `obj_inst_map` 为空的盘；已被任何盘 map 声明的实例跳过
  （有成员记录的盘行为原样，全部既有 fixture 零变化）
- 逐 printable 实例：凸包包围盒（世界坐标）XY 与候选盘网格矩形
  （新纯函数 `orca::compute_plate_box`，PlateGrid.hpp，SceneEpsilon 膨胀）
  相交测试，首个命中盘获胜；`printable="0"` 排除（桌面 unprintable 语义）
- enroll 沿用 P-22 模式：`loaded_id = id().id` + 递增 key
  → 6 个匹配点零改动；顺带绕开上游单键 map 多实例丢实例缺陷
  （bbs_3mf.cpp:4473，仅对无成员记录的盘生效）
- enroll > 0 时发 tip 级 `PLATE_MEMBERSHIP_REBUILT`（含 assigned/excluded
  计数）；重建后仍空的盘维持 P-19 warning+skip 不变

**Fix B（可观测性）**：`finalise_statistics` 空盘分支补 error 级
`NO_PLATES_PRODUCED` issue（plate_id=-1，不触发 placeholder 盘伪造）；
已有 error/serious_warning issue 归因时抑制（如
PLATELESS_NO_PRINTABLE_INSTANCES 已在场，通用码不加信息）。

## 验证证据

- 事故件手测（本机 build-static，2026-10-08）：修复前 rc=6 /
  warnings-only；修复后 rc=0、1 盘成功、`PLATE_MEMBERSHIP_REBUILT` tip、
  出产物 gcode.3mf 3.67MB、重量 31.74g（内嵌黄金 21.04g，U1 官方预设
  替换原机配置致偏，Case 26 以 5-40g 宽区间锚定，风格同 P-22 Case 20）
- e2e：Case 26（事故件成功路径）、Case 27（双盘重建 + 良性空盘并存，
  盘 2 仍 EMPTY_PLATE warning）、Case 17 更新（重生成 fixture 保持零产物
  契约 + 新增 NO_PLATES_PRODUCED 断言 + 重建 no-op 无 tip）；Cases 15/16
  不动，充当"混合 map"回归锁（盘 1 显式声明立方体，重建不得重assign）
- 全量套件绿（`python3 tests/run_tests.py build-static`，junit 落盘）

## 已证伪 / 否决的方案

- **消费端"空 map = 全选"兜底**（mark_printable_instances 里判空即全
  printable）：P-22 文档已证伪的 D3 同族方案——在消费点打补丁会波及
  全部 6 个匹配点、且无法区分"文件真没成员"与"盘真没对象"两种语义。
  本次修复留在数据层（载入后一次性重建），与 P-22 D1''' 同构。
- **升级 fork 导入器解析 `<assemble>`/新格式**：超出引擎侧修复半径
  （non-goal），且桌面自己也不靠该节判归属（assemble 处理器只回填
  transform/offset，bbs_3mf.cpp:4490-4517）。
- **桌面全量几何重建（每次载入都重建所有盘）**：会改变全部既有盘的
  归属来源（obj_inst_map 记录 vs 几何），回归面不可控；收敛为"仅空 map
  盘候选"。与桌面的已知分歧：实例同时落在候选盘与非候选盘格子时，
  桌面按全盘序首个命中、引擎按候选盘序——真实可达文件上等价（有成员
  却漏记自己盘上实例的文件本就是损坏输入）。
- **Fix B 无条件 push**：与 PLATELESS_NO_PRINTABLE_INSTANCES 双错误并
  列（该路径以 issue 而非 error_message 归因，能穿过
  `!m_stats.error_message.empty()` 早退守卫）——加"已有 error 级 issue
  则抑制"守卫后维持单一归因。

## Fixture 派生配方（zipfile 读改写，无现成脚本）

- `slice_membership_rebuilt_2plate.3mf`：以 `slice_empty_plate_skip.3mf`
  为源，删除 `Metadata/model_settings.config` 中唯一
  `<model_instance>...</model_instance>` 块（含前导缩进），其余 entry
  逐字节保留 → 两盘 map 皆空，立方体 (129.19,183.71) 落盘 1 格子
- `slice_all_plates_empty_fail.3mf`（重生成）：原文件
  `3D/3dmodel.model` 唯一 `printable="1"` → `"0"`。必须重生成：否则
  Fix A 把立方体 enroll 进盘 1，Case 17 翻绿丢失零产物回归覆盖
