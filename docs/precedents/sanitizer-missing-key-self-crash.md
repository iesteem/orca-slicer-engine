# P-29: 加固函数自身死于缺键输入（sanitize_layer_config_ranges）

**状态：** 已修复（feature-2.2.0，2026-10-20；外部审查 MR !3 F1 发现）
**Fixture：** tests/fixtures/layer_range_no_global_key.3mf（Case 32，
tests/test_slice_e2e.cpp）

## 现象

带 `layer_config_ranges` 且配置块**整体缺失 `layer_height` 键**的 hand-crafted
3MF，引擎 rc 139（SIGSEGV）。崩的不是 libslic3r，而是为防它而写的
`sanitize_layer_config_ranges()` 自己。

## 根因链

1. `m_config` 由 `Model::read_from_file(..., &m_config, ...)` 直接从 3MF 配置块
   填充——块里没写的键此时**不存在**
2. defaults 回填（`normalize_loaded_config` 的
   `apply_only(defaults, missing, true)`）在 `run()` 里**晚于**
   `load_3mf` → sanitize 执行时 `layer_height` 可以缺席
3. fork `Config.hpp:2361` const 版 `opt_float` 实现为
   `dynamic_cast<const ConfigOptionFloat*>(this->option(key))->value`，
   `option()` 缺键返回 `nullptr` → 空解引用
4. 同函数上一行对 `filament_colour` 恰好有 `colours ? ... : 0` 守卫——
   守卫不一致把问题藏在眼皮底下

## 修复

`src/SliceEngine.cpp` sanitize 头部改为 `option<ConfigOptionFloat>`
判空 + `FullPrintConfig::defaults()` 兜底（与片刻后 backfill 装进的
默认值同源，语义不变）。

## 验证铁律

- Case 24/25 的 fixture 配置块**携带** `layer_height`，恰好绕开此路径——
  "已有测试覆盖 sanitize" 不等于 "缺键路径已覆盖"；造变体必须动
  **配置块键集合**本身，不是 ranges XML
- 红→绿运行时实证：修复前新用例 SIGSEGV（Catch2 fatal error），
  修复后 4 断言过；`[layer_range]` 全族 4 用例 20 断言无回归
- 同型雷区：**任何在 `normalize_loaded_config()` 之前读 `m_config` 的代码，
  单键 `opt_*` 取值前必须判空**（`has()` 或 `option<T>()` 判空），
  `validate_printer_model` 的先 `has()` 后取值是正确范式

## 来源

外部审查报告（MR !3，林志强 + Claude Code，2026-10-16，快照 a6ef0a0f）
F1；三份报告交叉里 PR #10 两份均未发现此项——不同审查者互补。
