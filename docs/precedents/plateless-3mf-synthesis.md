# P-16: 无 plate 元数据 3MF 无法切片（假成功静默失败）

**状态：** 已修复（21b4ef1，feature-2.1.7-base，2026-09-24；ENGINE_VERSION 02.01.07）
**Fixture：** `tests/fixtures/plateless_single.3mf`（md5 `07633924413a0c1fda3d31a2fd88146b`）、
`plateless_zbake.3mf`（md5 `bf95d17d97dba5309e3f4a7cfb2b98e7`）、
`plateless_unprintable.3mf`（派生：item 加 `printable="0"`）、
`plateless_oob_fail.3mf`（78bed5012，越床分歧锚定）；e2e Case 17-20

## 现象

第三方导出 3MF（`slice_info.config` 仅 header、`model_settings.config` 无
`<plate>`、无 plate png；几何/对象/item 正常）：

- 日志链：`No plate data in 3MF, treating as single plate` →
  `Marked 0 instances printable` → `Skipping empty plate` →
  `No plates completed successfully`
- `plates: []`、零产物，且 **exit 0**（失败路径不上 issue、不置
  `m_any_error`，`exit_code()` 读到全零）——调用方按 rc==0 判成功会静默
  吞掉失败，比崩溃更难发现
- 桌面 Orca 2.4.0 对同一文件切片成功

## 根因

引擎把"3MF 必须自带 plate 元数据"当成隐式契约，兜底只补了盘壳、没补
成员关系：

1. BBS 导入器只从 model_settings.config 的 `<plate>` 建 plate_data
   （bbs_3mf.cpp:4426；slice_info 里的 `<plate>` 不建盘）→ `m_plate_data` 空
2. `validate_input()`（原 SliceEngine.cpp:1577）兜底 new 了一个
   **obj_inst_map 为空**的 PlateData
3. 引擎不用桌面那套几何归属，6 个实例匹配点全靠
   `inst->loaded_id ∈ obj_inst_map value.second` 成员判定
   （mark_printable_instances:2126、run_build_volume_check:2192/2212/2262、
   check_spiral_lift:2340、populate_plate_data_for_export:3486）；
   无盘文件 `loaded_id` 恒 0（唯一写点是导入器 bbs_3mf.cpp:2262）→ 0 成员
4. "Skipping empty plate" 仅日志 warning，不落 JSON、不置错误态 → exit 0

**桌面端语义（实证链）**：导入层产空 plate_data 零补偿
（bbs_3mf.cpp:1528-1537）→ GUI `PartPlateList::init()` 恒有默认盘
（PartPlate.cpp:3264-3293）→ 盘归属纯几何 `plate_box.intersects`
（PartPlate.cpp:4550-4592）→ item 无 `printable` 属性默认 true
（bbs_3mf.cpp:470-474）→ 切片过滤 `is_printable()`（PrintApply.cpp:145）。
**等效语义：无 plate = 单盘 = 全部 printable 床内实例。**

## 修复（D1'''，数据层补齐而非消费点分支）

`read_3mf_model()` 载入成功后，`m_plate_data` 空 → 合成
`PlateData{plate_index=0}`；每个 **printable** 实例入册：
`inst->loaded_id = inst->id().id` + `obj_inst_map.emplace(递增key, (j, id().id))`
→ 6 个匹配点零改动自动接通。

关键论证（全库核验过）：
- `id().id` 全局唯一、恒 > 0，与导出器 identify_id 的 fallback 取值相同
  （SaveStrategy 无 UseLoadedId 时写 `inst->id().id`，bbs_3mf.cpp:7776/7913）
  → libslic3r 所有 `(loaded_id>0) ? loaded_id : id().id` 分支前后全等，零副作用
- `printable="0"` 实例不入册 → 与桌面一致被排除，且不被
  mark(:2127)/build_volume(:2264) 两次覆写复活；未入册实例被三循环
  `!printable` 短路跳过，不触发 BUILD_VOLUME 误伤
- 无盘文件走 "other vendor" 导入分支（Application 不带 BambuStudio-/
  Snapmaker_Orca- 前缀）→ 多实例对象被拆单（bbs_3mf.cpp:1921-1964）；
  递增 key 保证即便 BBS 标记无盘文件带多实例对象也无覆盖
- 引擎/导出器从不读 obj_inst_map 的 key（只读 value.second），key 唯一性
  即充分

配套：
- `validate_input()` 空盘兜底 → `PLATE_DATA_MISSING` 硬不变量（按构造
  不可达的防御性报错，堵死 exit-0 无产物通道）
- 合成盘零成员 → `PLATELESS_NO_PRINTABLE_INSTANCES` error（exit 6）；
  **真实空盘维持 warning+skip**（P-19 选项 A：一个空盘不得拖死其余盘）
- JSON 发 tip 级 `PLATE_DATA_SYNTHESIZED`（云端区分真单盘与合成盘）

## 验证证据

- 全量套件绿（含新 Case 17-20，零回归；本分支 16 个既有 fixture 全有盘，
  不触发改动路径）
- 黄金基准（桌面 2.4.0 对同一文件 3ea6302e6648d5b3.3mf 的 gcode）：
  21.28 g **逐位一致**、68 层、max_z 13.65、无擦除塔、首条对象打印移动
  `G1 X29.248 Y46.425 F30000` **逐字节一致**（耗时 44:04 vs 44:21 为
  估算器 0.6% 差）
- 5 个真实无盘模型：4 个恢复产出；78bed5012 exit 6 属已知分歧（下节）

## 已证伪的推测（两轮对抗审查，防止重新走弯路）

1. **"只造盘 + 加合成盘标志分支"（D1 纯标志版）不可行**：合成盘
   `identify_ids` 恒空集，6 个匹配点逐一加分支漏一处即静默失效
   （首轮审查即杀）；数据层入册让分支归零。
2. **"消费点隐式'空 map = 全选' fallback"（D3）危险**：真实文件存在
   "单盘 + 空 obj_inst_map"（桌面导出空盘）；无条件 error 化会推翻
   feature-2.2.0 上 P-19 选项 A 用户决策（多盘 1 好 1 空 rc=0），把
   合法文件拖成 rc=6 零产物——error 必须以 `m_synthesized_single_plate`
   门控收窄。
3. **"obj_inst_map 单键 map 冲突阻塞伪造入册"不成立**：无盘文件走
   other-vendor 分支已把多实例对象拆单；递增 key 双保险。
   （注：BBS **真盘**同对象多实例确实被 emplace 静默丢条目——上游
   独立缺陷，见"遗留"，与本修复无关。）
4. **"Marked N / got M 口径异常是 loaded_id 机制坏了"**：误报。前者是
   identify 匹配计数、后者是纯几何计数（`update_print_volume_state`
   不查 printable），loaded_id 机制无恙。

## 遗留 / 已知分歧（有意不修，锚定于 Case 20）

- **合成盘成员越床仍走 BUILD_VOLUME 硬错误**，桌面为"排除该实例 +
  继续切其余"（GLCanvas3D `contained_min_one` 闸门）。
  `plateless_oob_fail.3mf`（78bed5012，obj4 xmin=0 < 床界 0.5）修复后
  仍 exit 6 属预期。若未来对齐桌面，需"合成盘（或一般化）排除+警告"
  方案单独评审——注意会与既有越界失败 e2e 语义冲突。
- bed_type 缺键回退分歧、无缩略图（`model_thumbnail` 空）、CLI `-p`
  本分支不限制切盘（single_plate 无生产赋值点，2.2.0 已修）。
- BBS 真盘同对象多实例静默丢实例（上游 obj_inst_map 单键 map，
  bbs_3mf.cpp:4473；crafted_dup 实验：双实例只切 1 个）——独立上游缺陷。
