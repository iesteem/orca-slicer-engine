# Changelog

> **Built version**: `02.01.06` — defined by `ENGINE_VERSION` in `CMakeLists.txt`.
> This is the single source of truth; the tests and `scripts/package_linux.sh` read
> the version from CMake. The entries below track the broader product release line.

### v02.01.11 (2026-06-12)

- **计划新增 `--threads <N>` 参数（尚未实现）**：计划限制 TBB 并行线程数，用于避免资源耗尽，
  拟通过 `tbb::global_control::max_allowed_parallelism` 实现，默认 0 表示使用全部核心。
  (ebcae02, 219b4f7) — 注：截至当前 `src/` 中尚未实现该参数。

- **新增 G-code 导出内存风险评估**：引入逐层评分机制，在执行 G-code 导出前评估内存风险。
  优化 `model_complexity.py`、`batch_benchmark.py`、`3mf_score.py` 工具脚本，
  基于 k3s 切片验证数据重新校准 `memory_risk` 阈值。
  (34a3373, fe6526b)

### v02.01.10 (2026-06-11)

- **JSON 字段重命名**：`plates[].filaments[].filament_id` → `plates[].filaments[].id`，
  同时将 `SliceOutputStats::FilamentDetail::filament_id` 成员变量同步改为 `id`。
  注意：libslic3r 的 `FilamentInfo::filament_id`（`std::string` 类型）保持不变。
  (fca9ee3, f685137)

- **基础版本**：UTF-8 BOM、C++ 风格转换、安全函数替换等代码规范修复。
  (1e6a69c, 8fccaef)

---

## 引擎开发线（feature 分支，未发布）

以下条目来自引擎侧 feature 分支开发，按时间倒序：

### 2026-09

- **高流量 flow-variant presets 端到端支持**（fdcff12）。切片变体 = 打印槽
  `filament_volume_type` 在 flow_support 中的索引；nozzle_volume_type 不参与。
  判定方法见 `docs/precedents/high-flow-verification.md`。
- **擦塔位置 clamp 缺失定位**（2026-09-09）：TOOLPATH_OUTSIDE 根因为引擎不移植
  桌面 GUI clamp 回写。未修复，见 `docs/precedents/wipe-tower-position-clamp.md`。

### 2026-08

- **Mini train city 双 bug 修复**：空壳内嵌预设过滤 + toolpath_outside 弧线
  插值点漏平移。产品定义变更：工艺预设必须替换成功，否则 exit 6。
- **删 trim（路径 B）落地**（02a4135）：多色判定完全交 libslic3r，修掉
  layer_config_ranges 误判与槽位 remap 副作用。见
  `docs/precedents/modifier-extruder-trim.md`。
- **高度范围换色擦塔修复**（上游 5cac51bbe5）：`Print::object_extruders()`
  补读 per-feature filament 键。见 `docs/precedents/wipe-tower-layer-config-ranges.md`。
- **toolpath_outside 无头移植**（2026-08-17）：`compute_toolpath_outside()`，
  含 moves 全局坐标减 plate origin 等三坑。见
  `docs/precedents/toolpath-outside-headless.md`。
- **测试框架接入**（2026-08-04）：tests/ 之前从未构建，接入 Catch2 v3.5.2，
  63+ 集成用例、11 维度 fixture 池全部接入。

### 2026-07

- **3MF 缺配置键崩溃修复**：FullPrintConfig::defaults() backfill。见
  `docs/precedents/crash-missing-config-keys.md`。
- **flush_volumes_matrix 维度规整**（fe5c61c）：修迷你蘑菇 ToolOrdering
  SIGSEGV。见 `docs/precedents/crash-flush-matrix-mismatch.md`。
- **all-or-nothing 打包确认**（2026-07-28）：设计决策，见
  `docs/precedents/packaging-all-or-nothing.md`。
- **Z-bake 修复 + 参数替换简化**（0773324，+224/-371）。
- **plate_width int 截断对齐**（refactor-02.01.01）：修 0.9mm 棋盘格原点偏差。

### 2026-06

- **inherits_group 配置解析端到端验证**（v02.01.03）：11 盘多色 filament 全部正确解析。
- **耗材统计修复**（feature-02.01.04）：4 盘非 0 槽全部正确。

---

## 正式发布线（已合入 main）

以下为已通过 PR 合入 `main` 的正式发布记录（Keep a Changelog 格式，英文原文保留）。
格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)，版本遵循
`MAJOR.MINOR.PATCH` 引擎版本方案（`02.01.xx`）。

## [v02.01.04] — PR #6

### Fixed
- **Removed filament trim; wipe-tower delegated to libslic3r.** The engine no
  longer calls `resize()` on the filament list — `ToolOrdering` in libslic3r
  owns the decision. Fixes a bug where `trim` without remap collapsed JSON
  statistics to slot-0 only, and avoids an `append_tcr` crash from a misjudged
  `has_wipe_tower`. (`02a4135`)
- **Count modifier-volume extruders** when deciding single-color trim, so a
  multi-color model with modifiers is no longer mis-detected as single-color
  and wrongly trimmed. (`a789686`)
- **Resolve `?:` type mismatch** in `assemble_plate_stats`. (`e5beb87`)

### Changed
- **Snapshot post-trim filament colour / type / nozzle / diameter / density**
  so JSON statistics match the slots actually used at print time. (`862b753`)
- **Decouple SEMM skip-trim from `used_extruders` stats set** — the two
  concerns are now tracked independently to avoid cross-contamination.
  (`272c553`)
- **Dropped duplicated post-trim snapshot block** in `export_gcode`. (`48069ee`)

## [v02.01.03] — PR #2 + PR #3

### Added
- **`inherits_group` resolution.** Parses the 3MF inheritance chain as
  `[process, fil_0..N-1, printer]` (N+2 elements), so keys like `fil_0` are
  recognised correctly. (`2febb62`)
- **Per-plate temperature-mixing check + correct slot retention on trim**,
  mirroring the desktop `check_filament_temp_mixing` behaviour — trim now
  keeps the correct slot rather than blindly resizing. (`17362fd`)
- **`--skip-preset-substitution`** CLI flag to bypass official preset
  enforcement. (`70f7225`)
- **Organic-supports and variable-layer-height detection** now raises an
  error (previously silent), migrated from string match to enum type.
  (`8f76a53`, `e2fb935`)

### Fixed
- **Thumbnail passthrough.** Thumbnails are read from disk during decode,
  fixing missing `; thumbnail` headers in emitted G-code. (`5399300`,
  `c3b3530`, `dde0ae8`)
- **`TOOLPATH_CONFLICT`** now uses `EXIT_POSTPROCESS_ERROR (7)` and sets
  `m_any_error` to block `gcode.3mf` generation. (`b1d7aae`, `f25b8d4`)
- **`PRINT_BY_OBJECT_CAUTION`** promoted from warning to error and now
  blocks `gcode.3mf` generation; message updated to English and references
  Snapmaker Orca. (`24a4f2e`, `275154e`, `8dedd3d`)
- **`PRESET_PRINTER_NOT_FOUND`** demoted to warning — there is a downstream
  fallback. (`cb9bae6`)
- Removed `fprintf(stderr)` calls that violated the coding standard. (`7b9f6bf`)

### Changed
- **God-class decomposition of `SliceEngine`.** `load_3mf` split into
  validate / read / sanitize; `apply_*_official_preset` extracted as
  dedicated helpers; `validate_presets` renamed to `collect_config_warnings`.
  Every function previously over 150 lines has been split. ~1790 lines of
  dead code (`PlateProcessor`, `StatisticsBuilder`) removed. (`c9fae36`,
  `927a19e`, `1bf94cf`, `ffb8e0c`, `2798813`, `29d0d4a`, etc.)
- **Unified preset-substitution reporting** across printer / filament /
  process. `PRINTER_SUBSTITUTED` now preserves the user's original preset
  name. (`e3167da`, `5d283d5`)
- **Severity clean-up.** `SPIRAL_LIFT_NEAR_BOUNDARY` demoted to warning
  (no longer blocks slicing with early return). (`343fa4b`, `043c1ec`)
- **Snapmaker-only packaging.** Third-party packages stripped; `--log-file`
  removed in favour of unified `--log`. (`70f7225`)

## [v02.01.01] — PR #1

### Added
- **Decoupled `ENGINE_VERSION`** from OrcaSlicer's version string.
  (`2fb040b`)
- **Linux packaging script** `scripts/package_linux.sh`. (`8b31045`)
- **`-j` / `--json` CLI option** for standalone slice-statistics JSON output.
  (`ac997a1`)
- **`--log` log-file output** with sensible default path derived from the
  output directory; supports both level mapping and a `--log-file` alias
  for backward compatibility. (`8ff0eb8`, `254652b`, `fc38b95`, `2f826b8`)
- **Model ensure-on-bed**: imported models are auto-positioned onto the bed.
  (`55490e9`, `e14cc26`)
- **Official U1 printer preset replacement** before slicing, so user
  printer config is overridden wholesale. (`7802c66`)
- **Official process preset application**, fixing brim / skirt mismatch
  with desktop. (`48d6e15`)
- **`ORCA_STATIC` static monolithic build mode.** (`bc6629e`)

### Fixed
- **Case-insensitive `.3mf` extension detection.** (`4268305`)
- **Z lift dropped by `Print::apply`** during ensure-on-bed, which truncated
  the model. (`98ea2b1`)
- **`--log` argument-parsing defect** that overwrote `input_3mf`; info-level
  log mapping; auto-path derivation now prefers the output directory over
  the `input_3mf` directory. (`254652b`, `fc38b95`, `2f826b8`)
- **Nozzle-suffix detection** migrated from fragile substring match to an
  independent token. (`87af1e6`)
- **`filament_roll_back`** no longer uses `orca_filament`. (`04e2e8c`)

### Changed
- **Filament-rollback strategy**: non-official filaments fall back to a
  base category, with unified full overwrite for printer/filament
  substitution. (`b34a914`, `67969ee`)
- **Unified JSON output to the `JsonReport` schema.** (`bc6629e`)
- **CLI argument handling aligned** between `main.c` and the C API.
  (`dc3f09f`)

---

Versions prior to v02.01.01 tracked OrcaSlicer's version directly and are
not listed here.
