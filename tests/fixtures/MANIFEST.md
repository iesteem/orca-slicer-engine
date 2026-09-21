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
| missing-keys_backfill_10632.3mf | P-01 defaults backfill |
| hollow_embedded_preset_train_city.3mf | P-15 空壳内嵌预设 → exit 6 |
| high_flow_whitelist.3mf | P-13 高流量生效 |
| cylinder_four_plate_mixed.3mf | 多盘混合成功/失败 |
| wipe_tower_toolpath_outside.3mf | P-08 擦塔越界现象 |
| wipe_tower_import_regen_ok.3mf | P-08 阳性对照：2c242c10f0e4b521 经 Orca「导入」重存（塔位 165/244.697→13/214.5 自动重生）→ exit 0 全成功。md5 `1fb1246b4f3d2aed4be0d58a70e17d58` |

### 子目录

`accuracy/ boundary/ build_volume/ build_volume_dir/ filament/ filement_count/
file_validation/ gcode_security/ layer_height/ path_conflict/ printer_reject/
print_sequence/ temp_mode/ unsupported/` —— 与同名 test_*.cpp 一一对应，
各文件头部注释记录实测行为（断言忠于实测，非想当然）。
