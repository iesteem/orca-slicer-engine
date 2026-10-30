# P-01: 3MF 缺 PrintConfig 键 → libslic3r null 解引用 SIGSEGV

**状态：** 已修复（`prepare_merged_config_for_plate` 内 FullPrintConfig::defaults() backfill）
**Fixture：** `~/Downloads/44ae4e9634d82e33.3mf`（外部，未入库）

## 现象

v02.01.04 切片崩溃，exit 139 (SIGSEGV)，崩在 `Print::apply`。

## 根因（ASan + GDB 反汇编确证）

3MF 的 `project_settings.config` **缺 `seam_slope_type` / `thumbnails` 等 PrintConfig
键**。`PrintApply.cpp:1320` 的 `get_config_value<ConfigOptionEnum<SeamScarfType>>`
在对象 config 与 global config 都缺键时返回 **nullptr** → `->value` 解引用
（成员偏移 8 → 崩溃地址 0x8）。修好 `seam_slope_type` 后崩到 `thumbnails`
（地址 0x10，同一类问题的多米诺）。

桌面版 GUI 永远持有完整 PrintConfig 所以不触发；无头引擎只拿 3MF 嵌入配置，
**必须自己补全缺失键**。

## 修复

`prepare_merged_config_for_plate`（merged_config = m_config 之后）backfill 所有
defaults 有而 merged_config 缺的键（`apply_only(defaults, missing, true)`），
已有值不动。fixture: `tests/fixtures/missing_keys_backfill_10632.3mf`。

## 已证伪的推测（勿重走）

- ❌ "extruder_colour 越界" —— 早期推测，非根因（数组对齐已实现但非崩因）
- ❌ "CGAL throw 段错误 / LTO ABI 损坏" —— `catch throw` 抓到的 CGAL 异常是
  被 `check_self_intersect` 正常捕获的红鲱鱼

## 教训

1. **不要相信 LTO 构建的 `.cold` 栈符号**（错位到异常路径）。用反汇编崩溃
   PC + 读寄存器 + `catch throw` 区分真 SEGV 与被 catch 的异常。
2. `mov 0x8/0x10,%reg` + `ud2` = null 对象成员解引用，不是异常 terminate。
