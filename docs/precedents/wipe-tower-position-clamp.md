# P-08: 擦塔位置 clamp 缺失 → 误报 TOOLPATH_OUTSIDE（未修复）

**状态：** 根因已定位（2026-09-09），**未修复**（v02.01.06 决定不处理；
WIP 存于 git stash `stash@{0}`，含直移植 clamp + WTCLAMP 探针）
**Fixture：** `tests/fixtures/2c242c10f0e4b521.3mf`（已入库，md5
`3dba3d258052e3d4503bd1514d15ce77`）；库内另有
`wipe_tower_toolpath_outside.3mf`

## 2026-09-09 移植尝试的关键发现（WIP 未完成的原因）

直接移植桌面 clamp（`y > bed_max − depth − margin` 则回推）**不够**：

- 引擎实测切片后真实 depth = **19.5**，但塔 gcode 实际 Y 向延展 ≈
  **26.5mm**（anchor 244.697 → 顶 271.21）——超出部分来自 brim(5) +
  塔底锥度扩展（cone base）。
- 引擎 printable_area bbox max.y = **271**，桌面渲染 clamp 用的 plate
  bbox（bed shape）max.y = **270**、depth 用估算值 **21.5**（估算路径
  prime_volume 30×3/(0.2×35)≈12.86 → min_depth_per_height 下限 20）——
  两套数字各自成立但都推到 242.5。引擎侧用"实测 depth 19.5 + 271"算
  出 270.2 ≤ 271 不触发 clamp，而真实塔顶 271.21 越界。
- **下一步**：clamp 的塔延展量不能只用 depth，需含 brim + cone base
  （参考 `Print::first_layer_wipe_tower_corners`，Print.cpp:2985-3005，
  `WipeTower2::get_wipe_tower_cone_base`），或直接用该 corners 多边形
  做 bbox 级 clamp。

## 根因

桌面 GUI 渲染塔预览时（GLCanvas3D.cpp:3856-3914）把塔位置 clamp 进盘内并
**回写** wipe_tower_x/y 到 project config。引擎无 GUI，盲信 3MF 的
wipe_tower_y（实测 244.697 vs 桌面 clamp 后 242.5），塔顶超界 0.21mm。

桌面 depth 计算：`estimate_wipe_tower_size`（PartPlate.cpp:1687）=
max(flush_depth, min_depth_per_height 插值)（{100→20, 250→40}，
WipeTower.cpp:602）；clamp 公式 `y = plate_max − depth − margin(1+brim)`。

## 修复方案（已定，未实施）

在 `process_plate` 前算 depth + clamp + 回写 m_config。验证标准：clamp 后
wipe_tower_y=242.5 且无 TOOLPATH_OUTSIDE。升级 libslic3r 无效——核心从不
做此事。完整调研：`~/Downloads/擦塔越界调研报告-2c242c10f0e4b521.md`。

## 注意

引擎曾有临时调试改动（ORCA_KEEP_FAILED_GCODE，SliceEngine.cpp 两处）
当时未提交未还原——接手时先检查。
