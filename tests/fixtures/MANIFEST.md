# Fixture Manifest

> fixture 是本项目"数据被继承"的一部分。本文件记录每个 fixture 的来源、
> 覆盖的判例（见 `docs/precedents/`）与外部依赖状态。

## 命名约定

| 类别 | 命名规则 | 理由 |
|---|---|---|
| 生产原件 | 保留生产系统原始文件名（内容哈希前缀），**禁止重命名** | 哈希名是判例文档与生产事故记录的引用锚点；与下表 md5 配合构成完整性校验；一眼可辨"不可再生成的真实样本" |
| 自制备/派生 fixture | 语义化命名，描述锚定行为或派生来源 | 可按 MANIFEST 登记的配方重新制备，名字即行为说明 |

新生产原件入库时：保留原名 + 在"已入库的判例 fixture"或对应分组
登记 md5 与判例编号；不得改成语义名（会断判例引用链）。

## 外部 fixture 池状态（2026-09-09 更新）

历史上的外部池 `~/Downloads/切片引擎测试-3MF文件包/` 已废弃：相应 3mf
已直接放入 `tests/fixtures/`（集成测试包），全部 in-repo。**新 fixture
一律进仓库，不再维护外部池。**

仍未入库的外部 fixture（判例文档中引用，崩溃根因已完整留档）：

| 文件 | 判例 | 状态 |
|---|---|---|
| 44ae4e9634d82e33.3mf | P-01 | 已不在本机，判例文档留存根因 |
| 迷你蘑菇.3mf | P-02 | 同上 |
| 5479_71a4b623998e7419.3mf | P-10 | 已不在本机（上游 bug，搁置中） |

已入库的判例 fixture：

| fixture | 判例 | md5 | 锚定行为 |
|---|---|---|---|
| 2c242c10f0e4b521.3mf (1.04MB) | P-08 | `3dba3d258052e3d4503bd1514d15ce77` | 擦塔位置超界 0.21mm → exit 7 TOOLPATH_OUTSIDE（未修复，修复后应转绿） |

## In-repo fixtures（tests/fixtures/）

### Z-variant / 几何预处理组（P-12 行为基线）

| fixture | 锚定行为 |
|---|---|
| geom_above_bed_ok.3mf | Z=13.5 → bake 后落床 min_z≈0 |
| geom_below_bed_mostly.3mf | 2 对象下沉保留（min_z<0），arrange_order 真验证 |
| geom_below_bed_oob.3mf | 同上 |
| geom_partial_sink_ok.3mf | 部分沉床保留（-3 < 0 < 24） |
| geom_partial_sink_oob.3mf | 同上 |
| geom_fully_below_bed.3mf | 全沉床抬升 + OBJECT_BELOW_BED_ADJUSTED warning |
| geom_straddle_empty_layers.3mf | 跨空层 |
| geom_too_high.3mf | 超高（build-volume 阶段，预处理不失败） |
| geom_defects_cabin.3mf | 几何缺陷全 warning 不阻断（SELF_INTERSECT 等） |
| slice_oob_fail.3mf / slice_too_high_oob_fail.3mf | e2e 越界/超高失败路径 |
| u1_preset_test.3mf | 单对象单实例，Z-bake 分支命中 |

### 配置/预设组

| fixture | 锚定行为 |
|---|---|
| 4e59b4a1a391c740.3mf (6.9M) | 对象级速度覆盖回归：support_speed 经 different_settings_to_system 下放 → findMaxSpeed 单参 opt_float null 解引用 SIGSEGV(139)；fork 656353d876 修复后 2 盘全成功 exit 0。md5 `d5907fe62760db57d9389980ca326c02` |
| missing-keys_backfill_10632.3mf | P-01 defaults backfill（注意：文件名是下划线 missing_keys…，历史上判例曾误写连字符） |
| thumbnail_short_png.3mf | 缩略图 PNG 仅 3 字节（<8 字节签名）→ 必须被尺寸守卫拒绝，不得进入 magic 比较越界读（geom_above_bed_ok.3mf 改造，2026-09-17） |
| hollow_embedded_preset_train_city.3mf | P-15 空壳内嵌预设 → exit 6 |
| u1_preset_test_truncated.3mf | C3 短侧截断：u1_preset_test 派生，6 个 per-filament 向量（filament_type/diameter/colour/nozzle_temperature×2/cool_plate_temp）截 2/4，filament_settings_id 保 4 槽 → 修复后全部归 4、槽位值=官方（u1_preset_test.3mf 解包重打包，仅编辑 project_settings.config，几何逐字节不变） |
| high_flow_whitelist.3mf | P-13 高流量生效 |
| geom_above_bed_support_on.3mf | JSON 支撑标记对照（配置开但零生成）：geom_above_bed_ok.3mf 解包重打包，仅 model_settings.config 插入对象级 `enable_support=1`（几何逐字节不变）→ `Print::is_support_used` 对象级覆盖路径仍走通（输出 3mf 盘元数据 support_used=true），但立方体贴床自支撑零支撑挤出 → stats JSON `support_free=true` / `plates[].support_free=true`（判定源 2026-10-10 换为 gcode 实际层；字段当日由 support_required/support_used 改名翻转，原 2026-10-07 落地） |
| geom_overhang_support_generated.3mf | JSON 支撑标记对照（真生成）：geom_above_bed_ok.3mf 解包重打包——对象网格替换为 T 形（立柱 13.5×13.5 从床起 + 顶帽 27×27，z 向重叠 1mm，16 顶点 24 三角形双盒网格）+ model_settings 对象级 `enable_support=1`；帽底 z=13.5 环形平底悬垂（0° < 默认阈值 30°）→ gcode 实际挤出支撑柱（实测含 `;TYPE:Support`）→ `support_free=false` / `plates[].support_free=false`（2026-10-10）。注：悬空实例 Z 造不出真支撑——ensure-on-bed 对单部件对象 `z_shift=-min_z` 贴床（SliceEngine.cpp bake_instance_z_into_mesh），故必须用真实悬垂几何 |
| mixed_filament_used.3mf / mixed_filament_defined_unused.3mf | 混色耗材定义vs使用对照（生产样本入库，2026-10-08）：两者 mixed_filament_definitions 逐字节相同（4 物理+3 enabled 行→虚拟 5/6/7）；仅 used 样本的对象 extruder=5/6/7。used→`mixed_filament_used=true`+gcode 展开 T0-T3；unused→false。注：面片 paint_color 与高度换色通路两样本均无数据，未覆盖 |
| mixed_filament_modifier_used.3mf / mixed_filament_painted_used.3mf | 混色使用通路补充（生产样本，2026-10-08）：modifier 样本对象+ParameterModifier part 均带 extruder=6（volume 级通路）；涂色样本 extruder 全 0、使用仅在 mesh paint_color 面片流（used_states 通路，实测导入重建正确）。两者 `mixed_filament_used=true` 且多物理槽展开（4/3 槽）。注：高度换色不支持混色耗材（GUI 仅物理耗材，用户确认 2026-10-08），layer_config_ranges 通路已从判定中移除 |
| mixed_filament_two_plate.3mf (307K) | 混色**多盘聚合**语义（2026-10-13，md5 `9eec39083b4fcd2e355e295cc8968b61`）：mixed_filament_used.3mf 派生——物理对象 id=2 的 build item 平移到盘 2 格心 (459,135)（2 盘网格 cols=2）+ model_settings 实例引用改filed 到新盘 2 块，其余 entry 逐字节保留（重打包统一了 local 段乱码文件名编码，引擎读 central 无碍）。盘 1 = 虚槽引用（4 槽展开）、盘 2 = 纯物理（1 槽），P-24 几何重建归属。实测锚定：`mixed_filament_used=true`——**文件级 OR 聚合**，任一盘引用虚槽即 true，纯物理盘不拖低（无 AND 语义）；两盘均 success |
| layer_range_missing_height.3mf / layer_range_oob_filament.3mf | 高度换色恶意输入回归（geom_above_bed_ok.3mf 注入 layer_config_ranges.xml，2026-10-08）：missing_height 缺 layer_height 键，修复前 Slicing.cpp:184 null-deref SIGSEGV(rc139)，sanitize_layer_config_ranges 补全局默认后 rc=0+CONFIG_INVALID_LAYER_RANGE；oob_filament 携带 wall/sparse_infill_filament=5（>4 物理），libslic3r 静默 clamp 槽 1，引擎补告警并锁定 mixed_filament_used=false |
| layer_range_valid_boundary.3mf | 正常 ranges 反告警（Case 31，2026-10-10 回归审查盲区②）：geom_above_bed_ok.3mf 注入完整边界 range——wall=1 首槽 / sparse_infill=4==num_physical 末槽（`>=` off-by-one 磁铁）/ solid_infill=0 合法默认 / layer_height 在 → 必须 rc=0 且零 CONFIG_INVALID_LAYER_RANGE，且 wall 槽与末槽有实测用量（ranges 真生效非空过）；同日 [filcount] multicolor 用例补内部值反告警断言 |
| u1_preset_test_oversized.3mf | P-21 shrink 侧（2026-10-10 回归审查盲区①，normalize 唯一无锚定路径）：u1_preset_test_truncated 派生，5 个 pass-1 core 键（retraction_length/z_hop/wipe/filament_diameter/filament_colour）拉伸至 6、filament_settings_id 保 4 → normalize 后必须裁回 4、flush 36→16。注意 pass-2（Preset::filament_options）grow-only 不裁长向量是桌面 parity 设计，勿对 flow-variant 键（nozzle_temperature*）断 shrink |
| cylinder_four_plate_mixed.3mf | 多盘混合成功/失败 |
| wipe_tower_toolpath_outside.3mf | P-08 擦塔越界现象 |
| wipe_tower_import_regen_ok.3mf | P-08 阳性对照：2c242c10f0e4b521 经 Orca「导入」重存（塔位 165/244.697→13/214.5 自动重生）→ exit 0 全成功。md5 `1fb1246b4f3d2aed4be0d58a70e17d58` |

### 无 plate 元数据组（D1''' 合成盘行为，2026-09-24）

| fixture | 锚定行为 |
|---|---|
| plateless_single.3mf (25KB) | 第三方导出（slice_info 仅 header、无 model_settings `<plate>`、Application="Snapmaker Orca" 走 other-vendor 拆分）；合成单盘全 printable 实例 → exit 0、plates:[1]。桌面黄金基准（Orca 2.4.0 同文件 gcode）：1 对象、中心 (71.25,37)、68 层、13.65mm、21.28g、无擦除塔。md5 `07633924413a0c1fda3d31a2fd88146b` |
| plateless_zbake.3mf (11KB) | 同上 + instance tz=48.613 命中 Z-bake Step-1（世界 z[0,97.23]），验证合成盘与 Z-bake 叠加路径。md5 `bf95d17d97dba5309e3f4a7cfb2b98e7` |
| plateless_unprintable.3mf (25KB) | plateless_single 派生：item 加 printable="0" → 全文件无可打印实例 → exit 6 + PLATELESS_NO_PRINTABLE_INSTANCES error |
| plateless_oob_fail.3mf (93KB) | 4 对象无盘文件，obj4 xmin=0 < 床界 0.5 → 合成盘成员 PARTLY_OUTSIDE 走既有 BUILD_VOLUME 硬错误（与桌面"排除续切"的已知分歧，锚定现行为） |

### temp_mode 组（高低温混用，2026-09-18 补齐 C1 漏检面）

来源：桌面 U1 工程制备（火烈鸟 11 部件基型，网格已替换为包围盒立方体——
配置层与原件逐字节一致，13MB→457KB）。统一基线：2 槽 = Generic PLA(低温)
+ Generic ABS(高温)、全局 feature 键全=1、对象显式 extruder=1。

| fixture | 混用来源（唯一 ABS 引用） | 预期（对齐桌面 Plater.cpp:22250-22276） |
|---|---|---|
| obj_feature_sparse/wall/solid_infill_abs_fail.3mf | 对象级 feature 键=2 | **未修复应转红**：当前放行，修复后 FILAMENT_TEMP_MIXING |
| part_feature_sparse/solid/wall_abs_fail.3mf | 零件级 feature 键=2 | 同上 |
| modifier_extruder_abs_fail.3mf | modifier 的 extruder=2 | 同上 |
| unused_abs_slot_ok.3mf | 无（ABS 槽闲置） | 永远成功（反误杀） |
| obj_feature_same_temp_ok.3mf | 对象级键=2 但双槽均 PLA | 永远成功（按槽温度分类） |
| multi_plate_obj_extruder_fail.3mf | 盘1对象级 extruder=2 | 当前已阻断盘1（归因回归，修复不得破坏） |

注：`obj_feature_*` 早期版本把键设在全局且 `different_settings_to_system`
为空，被 process 预设替换覆写回官方值（等价闲置槽场景），已作废由上述
版本替换；全局键要存活必须基于官方预设直接改键（参见既有
pla_abs_tower_conflict_fail.3mf 的 diff 集记录）。

### 子目录

`accuracy/ boundary/ build_volume/ build_volume_dir/ filament/ filement_count/
file_validation/ gcode_security/ layer_height/ path_conflict/ printer_reject/
print_sequence/ support/ temp_mode/ unsupported/` —— 与同名 test_*.cpp 一一对应
（support/ 例外：用例在 test_c_api.cpp 的 [support] 家族），
各文件头部注释记录实测行为（断言忠于实测，非想当然）。

### 空盘行为组（C2 修复，2026-09-20）

来源：geom_above_bed_ok.3mf 解包改造——仅编辑 Metadata/model_settings.config
的 `<plate>` 块（追加无 `<model_instance>` 子块的盘 2 / 删除盘 1 的引用），
几何与配置层逐字节不变。

| fixture | 盘构成 | 预期 |
|---|---|---|
| slice_empty_plate_skip.3mf | 盘1=立方体，盘2=空 | 多盘：rc=0、success=true、EMPTY_PLATE warning（归因盘2）、好盘正常打包；单盘（C API plate_id=2）：rc=6、success=false、零产物（Case 15/16） |
| slice_all_plates_empty_fail.3mf | 两盘皆空 | rc=6、success=false、两条 EMPTY_PLATE 各归因其盘、零产物 + NO_PLATES_PRODUCED error（Case 17；P-23 Fix B）；2026-10-08 重生成（md5 `22b628d983847e41fe5aef2e7e602f8d`，build item 改 `printable="0"`）——P-23 几何重建上线后 printable="1" 立方体会被 enroll 进盘 1 翻绿，重生成保持零产物契约 |

### 盘成员缺失组（P-23 几何重建，2026-10-08）

来源：MakerWorld / BambuStudio-02.06 / OrcaSlicer 2.4.2 新格式——盘元数据
只写盘壳不写 `<model_instance>` 成员；引擎按桌面 `reload_all_objects` 语义
几何重建归属（SliceEngine::rebuild_plate_membership）。派生配方见
docs/precedents/makerworld-membership-rebuild.md。

| fixture | 盘构成 | 预期 |
|---|---|---|
| 1e9d9919e7955ce4.3mf (3.5MB) | 1 盘无成员记录，生产原件（md5 `70cd1bcba83e12d21477ee074b01b0f0`） | 几何重建 enroll 唯一实例 → rc=0、1 盘成功、PLATE_MEMBERSHIP_REBUILT tip、出产物、重量 5-40g（内嵌黄金 21.04g，U1 预设替换致偏）（Case 26） |
| slice_membership_rebuilt_2plate.3mf | slice_empty_plate_skip 派生（删唯一 `<model_instance>`，md5 `918f7b5a34631be912685e522545bb57`）：两盘皆无成员 | 立方体几何归盘 1、盘 2 真空 → rc=0、1 盘成功、tip + 盘 2 EMPTY_PLATE warning（Case 27；同时是 Cases 15/16 的混合 map 回归对照） |

### membership 错位组（P-24 always-rebuild，2026-10-10）

来源：生产原件（第三方导出器产出，260mm 热床网格排版 + 自定义工艺预设
"Litos"）——model_settings.config 的 plate membership 与几何格位错位一格
（top.stl 记盘 3 但落在盘 2 格位、side.stl 记盘 5 落盘 3 格位、
orangeQuestion.stl 记盘 8 落盘 5 格位；对象按 260 床×1.2=312mm 网格排版，
当前 U1 打印机为 270 床）。P-23 Fix A 逐字信任非空 map 误判 3 盘
BUILD_VOLUME_FULLY_OUTSIDE（rc=6 零产物）；桌面 Orca 2.4.0 按
reload_all_objects 几何重建全 8 盘切片成功。判例：
docs/precedents/always-rebuild-plate-membership.md。

| fixture | 盘构成 | 预期 |
|---|---|---|
| 2d986d79661ffb62.3mf (286K) | 8 盘生产原件（md5 `0794e9323efb3fe073c603c6b493c951`），盘 2 元数据空但几何上 top.stl 落其格位 | always-rebuild 几何重排 → rc=0、8 盘全部 success、PLATE_MEMBERSHIP_REBUILT tip、出产物、逐盘耗材/时间非零（Case 28；桌面对照输出 plate_1..8 对象序列 black/top/side/marcoLamparaAjustado/orangeQuestion/marcoLampara/marcoLamparaInferior/marcoLamparaSuperior） |
| slice_offgrid_sole_fail.3mf | 桌面 U1 制备（md5 `4fa980de7db6edf17d20e51acee3b08d`）：单盘无成员记录，立方体实例在 (-53,155)，网格 bbox X[-66.5,-39.5] 全负、不交任何盘格 | 唯一实例被几何重建 drop → INSTANCE_NOT_ON_ANY_PLATE warning + EMPTY_PLATE(盘1) + NO_PLATES_PRODUCED error、rc=6、零产物；rc=6 源自零产物守卫而非警告本身（Case 30） |
| slice_offgrid_sibling_ok.3mf | 上件派生（md5 `cbc034407470455a71146d12d8278bd8`）：build 追加第二实例于盘 2 格心 (459,135)（2 盘网格 cols=2）+ model_settings 追加无成员盘 2 块，其余 entry 逐字节保留；配方详见下方 | 非阻断契约：网格外实例 warning 不拖垮好盘 → rc=0、盘 2 成功出产物、INSTANCE_NOT_ON_ANY_PLATE warning + 盘 1 EMPTY_PLATE warning + tip 三者并存（Case 29） |

slice_offgrid_sibling_ok.3mf 派生配方（Python zipfile 读改写，无现成脚本）：
① `3D/3dmodel.model` 的 `<build>` 内追加
   `<item objectid="2" p:UUID="00000002-b1ec-4553-aec9-835e5b724bb5" transform="1 0 0 0 1 0 0 0 1 459 135 13.5" printable="1"/>`
   （UUID 末位 +1 保证唯一；459/135 = 2 盘网格 cols=2 时盘 2 cell [324,594]×[0,270] 的中心）
② `Metadata/model_settings.config` 在盘 1 `</plate>` 后追加无成员、无缩略图引用的盘 2 块（plater_id=2 + locked/filament_maps）
③ 其余 entry（含乱码本地文件名的 `3D/Objects/立方体_1.model`）逐字节保留——重打包会修复 zip 文件名编码，改变 fixture 特征

### 支撑语义组（P-25 gcode-actual 判定，2026-10-13）

来源：生产原件语义化改名入库（同一小船模型桌面双保存，支撑开关为唯一
变量 + 立方体开支撑对照），构成"真实几何 × 配置正交"三件套。与根级
geom_above_bed_support_on.3mf / geom_overhang_support_generated.3mf
（自制简化几何）互补：这组走真实桌面保存的支撑配置链 + 2.5M 真实网格
曲面悬垂。用例：test_c_api.cpp "real-world boat pair + cube"。

| fixture | md5 | 锚定行为 |
|---|---|---|
| support/boat_support_off.3mf (2.5M) | `c0c62d74927c85f4edb11fe86c90ead2` | 支撑关 → gcode 零支撑挤出 → JSON support_free=true（双层）+ 3MF support_used=false |
| support/boat_support_on.3mf (2.5M) | `fa95509eede8ae2c326a274481762375` | 同一船、支撑开 → 真实生成支撑（采集时 ~1550 段 TYPE:Support，耗材 11.1g→19.1g）→ support_free=false + 3MF support_used=true |
| support/cube_support_on.3mf (24K) | `3faae135b7689a45f14fe8ba7dc7b109` | 立方体开支撑但自支撑几何 → 零支撑体积 → support_free=true + support_used=false——钉死**配置层不参与判定**（P-25 判定源 = gcode 实际层） |
