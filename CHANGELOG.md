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

- **无 plate 元数据 3MF 支持（D1'''）**：第三方导出的 3MF（slice_info 仅
  header、无 model_settings `<plate>`）此前一律 "Skipping empty plate" 静默
  失败。现按桌面 Orca 语义合成单盘：全部 printable 实例入册
  （`loaded_id = id().id`，与导出器 identify_id 写出值一致，6 个匹配点零
  分支接通）。`printable="0"` 实例不入册（桌面同排除）。配套：validate_input
  空盘兜底改为 `PLATE_DATA_MISSING` 硬不变量；合成盘零可打印实例报
  `PLATELESS_NO_PRINTABLE_INSTANCES` error（真实空盘维持 warning+skip，守
  P-19 选项 A）；JSON 增 `PLATE_DATA_SYNTHESIZED` tip。已知分歧（锚定）：
  合成盘成员越床仍走 BUILD_VOLUME 硬错误，桌面为排除续切
  （plateless_oob_fail.3mf）。测试 Case 17-20。见
  `docs/precedents/plateless-3mf-synthesis.md`（P-16）。
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
