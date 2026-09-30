# P-07: toolpath_outside 无头移植（三坑 + mini train city 连环 bug）

**状态：** 已修复（2026-08-17/18，feature-02.01.05）
**Fixture：** `toolpath_outside_mixed_7855.3mf`（in-repo）；10514（大文件，已归档）

## 三个无头环境特有的坑

1. **死分支根因**：`GCodeProcessorResult::toolpath_outside` 全仓库唯一写入方
   是 GUI 层 GCodeViewer.cpp:2419；libslic3r 无头路径只在 `reset()` 置 false。
   引擎必须自己移植生产端（`compute_toolpath_outside()`，移植
   GCodeViewer.cpp:2354-2419 的检测逻辑）。
2. **printable_area 不被填充**：导出路径走 `GCodeProcessor::apply_config(const
   PrintConfig&)`（只填 printable_height）；填 printable_area 的是
   DynamicPrintConfig 重载（只有 G-code 查看器用）。床多边形必须从引擎
   m_config 取。
3. **moves 是全局网格坐标**：`store_move_vertex` 加回 plate offset，而 gcode
   文本坐标是盘局部的。检测前必须把 moves 减 plate origin，否则非原点盘
   全部误报。**且 `interpolation_points` 也要同步 shift**（mini train city
   bug 2：弧线插值点漏平移 → 盘 2/3/4 全误报）。

## 附带修复

`assemble_plate_stats` 失败盘提前 return 导致 `plate_stats.toolpath_outside`
恒 false——填充必须移到 early return 之前。

## 教训

**移植桌面消费端代码时，必须追生产端在桌面哪一层**——GUI 填充的字段在
无头路径恒为默认值。同类死代码：geometry check 的 has_geom_error 阻断分支。
另：只断言 exit code/结构的冒烟测试会漏掉这类语义 bug。
