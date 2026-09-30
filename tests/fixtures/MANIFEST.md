# Fixture Manifest

> fixture 是本项目"数据被继承"的一部分。本文件记录每个 fixture 的来源、
> 覆盖的判例（见 `docs/precedents/`）与外部依赖状态。

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
print_sequence/ temp_mode/ unsupported/` —— 与同名 test_*.cpp 一一对应，
各文件头部注释记录实测行为（断言忠于实测，非想当然）。

### 空盘行为组（C2 修复，2026-09-20）

来源：geom_above_bed_ok.3mf 解包改造——仅编辑 Metadata/model_settings.config
的 `<plate>` 块（追加无 `<model_instance>` 子块的盘 2 / 删除盘 1 的引用），
几何与配置层逐字节不变。

| fixture | 盘构成 | 预期 |
|---|---|---|
| slice_empty_plate_skip.3mf | 盘1=立方体，盘2=空 | 多盘：rc=0、success=true、EMPTY_PLATE warning（归因盘2）、好盘正常打包；单盘（C API plate_id=2）：rc=6、success=false、零产物（Case 15/16） |
| slice_all_plates_empty_fail.3mf | 两盘皆空 | rc=6、success=false、两条 EMPTY_PLATE 各归因其盘、零产物（Case 17） |
