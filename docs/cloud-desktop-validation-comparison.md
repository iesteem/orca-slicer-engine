# 云引擎 vs 桌面端校验对比

## 目录

1. [打印机类型校验](#1-打印机类型校验)
2. [打印机预设替换（全量覆盖）](#2-打印机预设替换全量覆盖)
3. [耗材预设替换（全量替换）](#3-耗材预设替换全量替换)
4. [前处理校验（桌面端一致性）](#4-前处理校验桌面端一致性)
5. [后处理校验（桌面端一致性）](#5-后处理校验桌面端一致性)

---

## 1. 打印机类型校验

### 云引擎

**文件**：`src/PresetManager.cpp:598-625`、`src/SliceEngine.cpp:1011-1038`（两份相同实现）

硬编码检查：
- `printer_model` 缺失 → 错误 `PRINTER_MODEL_MISSING`，阻断切片
- `printer_model != "Snapmaker U1"`（严格相等）→ 错误 `PRINTER_MODEL_UNSUPPORTED`，阻断切片
- 阻断方式：`return false` → 管道中止，GCode 不会生成

### 桌面端

**无等效硬编码阻断**。桌面端接受任何打印机型号。

Snapmaker U1 检测仅用于 UI 定制（非阻断）：
- `boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1")`（大小写不敏感子串匹配）
- 影响：限制热床类型选项、隐藏部分温度选项、按对象打印时显示警告对话框
- 文件：`Plater.cpp:10431-10453`、`Tab.cpp:2740-2793, 3170-3207, 4034-4084`

### 关键差异

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 硬阻断非U1打印机 | 是 | 否 |
| 匹配方式 | 严格相等 `== "Snapmaker U1"` | 大小写不敏感子串 |
| 阻断行为 | 管道中止 | 从不阻断，仅 UI 调整 |
| 架构定位 | 单打印机网关 | 多供应商兼容系统 |

---

## 2. 打印机预设替换（全量覆盖）

### 云引擎

**文件**：`src/PresetManager.cpp:671-778`

**全量强制覆盖策略**：

1. 从 `nozzle_diameter[0]` 推导官方预设名：`"Snapmaker U1 (0.4 nozzle)"`
2. 从磁盘加载 JSON：`resources/profiles/Snapmaker/machine/Snapmaker U1 (0.4 nozzle).json`
3. `overwrite_all_keys_from()` — 无条件覆盖 `official_cfg` 中的**每个**键到 `m_ctx.config`
4. `overwrite_gcode_keys_from()` — 无条件覆盖所有 G-code 键（`machine_start_gcode`、`machine_end_gcode` 等 9 个）
5. `printer_settings_id` 和 `printer_model` 被替换为官方值
6. 验证 `printable_area` 和 `printable_height` 不再为默认值（否则报 `PRINTER_PRESET_NOT_APPLIED`）

**G-code 键列表**（`PresetManager.cpp:59-65`）：
```
machine_start_gcode, machine_end_gcode, before_layer_change_gcode,
layer_change_gcode, change_filament_gcode, machine_pause_gcode,
template_custom_gcode, printing_by_object_gcode, time_lapse_gcode
```

### 桌面端

**文件**：`PresetBundle.cpp:2603-2841`、`Preset.cpp:2029-2058`

**选择性合并策略**（基于 `different_settings_to_system`）：

1. 3MF 携带 `different_settings_to_system` — 用户修改过的键列表
2. `PresetCollection::load_external_preset()` 中：
   - 对于每个不在 `different_settings_list` 中的键 → **恢复为当前系统预设值**
   - 对于在 `different_settings_list` 中的键 → **保留**用户的 3MF 修改值
3. `ignore_settings_list = {"inherits", "print_settings_id", "filament_settings_id", "printer_settings_id"}` — 这些键不被视为"脏"
4. 修改后的 G-code 显示警告对话框但不覆盖

### 关键差异

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 覆盖策略 | 来自 JSON 的**全量强制覆盖** | 基于 `different_settings` 的**选择性合并** |
| 配置源 | 磁盘上的官方预设 JSON | 内存中的 SystemPresetCollection |
| 用户修改的 G-code | **丢弃**（始终用官方覆盖） | **保留**（如果被标记为已修改） |
| `different_settings_to_system` | 未使用 | 用于决定保留哪些键 |

### G-code 警告的不一致

云引擎 `validate_presets()` 在检测到修改后的 G-code 时发出警告 "retained as-is (desktop parity)"，但由于后续的 `apply_printer_official_preset()` 无条件覆盖了 G-code，**实际上并未保留**。这是一个文档/行为不一致问题。

---

## 3. 耗材预设替换（全量替换）

### 云引擎

**文件**：`src/PresetManager.cpp:356-596`（`validate_filament_official()`）、`src/PresetRollback.cpp:1-213`

函数对每个挤出机索引 `i` 执行，`i` 从 `filament_settings_id` 向量的 `0` 到 `num_filaments-1`。

#### 完整决策流程

```
filament_settings_id[i] = "Some PLA"          ← 3MF 中的耗材预设名
         │
         ▼
   find_in_system(name)                        ← 在 Snapmaker + OrcaFilamentLibrary 中查找
         │
    ┌────┴────────────────────────────┐
    │ 找到                            │ 未找到
    │ is_official_preset()?           │ find_in_project(name)
    │                                 │   ┌─ 找到（项目嵌入预设）→ current = 该项目预设
    │  官方 → skip，不做任何替换       │   └─ 未找到 → current = nullptr
    │  非官方 → current = 该系统预设   │
    └────────┬────────────────────────┘
             │
    ┌────────┴─────────────────────────────────────────────┐
    │ current == nullptr?                                  │
    │                                                     │
    │  YES → 预设名在任何地方都不存在                        │
    │    enforce=false → 检查 has_inline_filament_config() │
    │      有内联配置 → FILAMENT_CUSTOM_INLINE warning     │
    │      无内联配置 → PresetRollback (调用点1)            │
    │    enforce=true  → PresetRollback (调用点1)          │
    │      rollback 成功 → 用基础耗材替换，continue         │
    │      rollback 失败 → FILAMENT_NO_OFFICIAL_ANCESTOR   │
    │                      terminal error，阻断切片         │
    │                                                     │
    │  NO → 预设存在但非官方                                │
    │    enforce=false → 沿 inherits 链做校验(warning)     │
    │                    不替换任何值                       │
    │    enforce=true  → 进入双路径替换                     │
    └──────┬──────────────────────────────────────────────┘
           │ enforce=true, current != nullptr
           ▼
    ┌──────────────────────────────────────────────────────┐
    │               Path 1: inherits 链查找                │
    │                                                      │
    │  while (current && !resolved) {                      │
    │      inherits_name = current->inherits()             │
    │                                                     │
    │      if (inherits_name.empty())  break;  ──┐         │
    │      if (循环引用)  error, break;           │         │
    │                                             │         │
    │      parent = find_ancestor(inherits_name)  │         │
    │      if (!parent) break;  ──────────────────┤         │
    │                                             │         │
    │      if (is_official(parent))                │         │
    │          → substitute_filament_params()      │         │
    │          → resolved = true, done             │         │
    │                                             │         │
    │      if (parent 不是耗材类型)                 │         │
    │          → FILAMENT_UNKNOWN_ANCESTOR error   │         │
    │                                             │         │
    │      else current = parent;  ← 继续向上走   │         │
    │  }                                           │         │
    └──────────────────────┬───────────────────────┘         │
                           │ 三种退出情况：                   │
                           │ ① inherits 为空 ←──────────────┘
                           │ ② parent 找不到 ←──────────────┘
                           │ ③ 循环引用(error)
                           │ resolved == false
                           ▼
    ┌──────────────────────────────────────────────────────┐
    │          Path 2: PresetRollback 兜底 (调用点2)        │
    │                                                      │
    │  不依赖 inherits 链。直接从 m_config 读取：           │
    │    filament_type[i]    = "PLA"                       │
    │    filament_vendor[i]  = "Snapmaker"                 │
    │                                                      │
    │  Pass 1: 同供应商匹配 (find_in_vendor)               │
    │    筛选：vendor 匹配 && filament_type 匹配            │
    │    评分：含"Base" +100, 非实例化 +80,                 │
    │          inherits fdm_filament_* +60,                │
    │          名称无喷嘴直径 +40                           │
    │                                                      │
    │  Pass 2: 通用匹配 (find_generic)                     │
    │    筛选：不限 vendor                                  │
    │    评分：OrcaFilamentLibrary +100,                   │
    │          含"Generic" +80, 非实例化 +60,              │
    │          inherits 为空(root) +50,                    │
    │          inherits fdm_filament_common +40            │
    │                                                      │
    │  rollback 成功 → 用基础耗材全量替换，resolved = true  │
    │  rollback 失败 → FILAMENT_NO_OFFICIAL_ANCESTOR       │
    │                  terminal error，阻断切片             │
    └──────────────────────────────────────────────────────┘
```

#### 关键设计规则

1. **inherits 链优先，PresetRollback 兜底**：PresetRollback 永远是最后方案，不存在跳过 inherits 链直接走 PresetRollback 的路径
2. **PresetRollback 的两个调用点**：
   - 调用点1（行 435）：预设名不存在（`current == nullptr`）——唯一方案
   - 调用点2（行 537）：inherits 链走完未找到官方祖先——兜底方案
3. **`inherits` 字段来源**：预设对象的元数据，在加载 3MF 时从嵌入预设的 `inherits` 字段或预设 JSON 的 `"inherits"` 键读入，不是从 `filament_settings_id` 推导的
4. **`substitute_filament_params()`**（行 564-596）：无条件覆盖该挤出机的所有向量键值（`nozzle_temperature`、`filament_diameter`、`filament_type` 等），同时将 `filament_settings_id[i]` 替换为官方预设名

#### 多挤出机处理

耗材数量超出打印机挤出机数量（由 `nozzle_diameter` 大小决定）时裁剪，发出 `FILAMENT_COUNT_MISMATCH` 警告。

### 桌面端

**无耗材替换机制**。桌面端接受任何耗材预设，不进行自动替换。

- `validate_preset()` 仅检查预设是否存在于系统/默认预设中（警告，不阻断）
- 修改后的 G-code 显示对话框，允许用户保留
- `is_filament_compatible()` 仅用于多材料兼容性检查，不用于预设替换

### 关键差异

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 替换策略 | **全量覆盖**为官方耗材 | **不替换** |
| 查找机制 | inherits 链 + PresetRollback 双路径 | 仅 `validate_preset()` 存在性检查 |
| PresetRollback | 两阶段评分搜索（同供应商→通用） | 不存在 |
| 阻断行为 | 强制模式下非官方耗材阻断切片 | 从不阻断 |
| 自定义耗材 | 仅 `--allow-custom` 下允许 | 始终允许 |

### 云引擎中的代码重复

云引擎存在两份 `validate_filament_official()` / `substitute_filament_params()`：
- `PresetManager.cpp` — 活跃版本（全量覆盖 + PresetRollback）
- `SliceEngine.cpp` — 过渡版本（仅 nil-fill，无 PresetRollback，**未被 `run()` 调用**）

---

## 4. 前处理校验（桌面端一致性）

### 共享代码：`Print::validate()`

云引擎和桌面端使用**完全相同的** `Print::validate()` 实现（来自 `libslic3r` 静态库）。

#### 完整的 `Print::validate()` 检查项

| # | 检查项 | 严重级别 | 阻断？ | Print.cpp 行号 |
|---|--------|----------|:---:|---------------|
| 1 | 无对象 (`m_objects.empty()`) | Error | 是 | 1553 |
| 2 | 无挤出机 (`extruders.empty()`) | Error | 是 | 1557 |
| 3 | 多耗材温度不匹配 | Error | 是 | 1559-1566 |
| 4 | 延时摄影 + 按对象打印 | Error | 是 | 1569-1570 |
| 5 | 顺序打印碰撞（`STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT`） | Error | 是 | 1572-1577 |
| 6 | 层打印碰撞（`STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT`） | Warning | 否 | 1580-1586 |
| 7 | 螺旋花瓶多副本 | Error | 是 | 1588-1594 |
| 8 | 螺旋花瓶多材料 | Error | 是 | 1596-1604 |
| 9 | 超出构建体积高度 | Error | 是 | 1623-1651 |
| 10 | 可变层高 + 树状支撑 | Error | 是 | 1658-1668 |
| 11 | 擦拭塔：不同喷嘴/耗材直径 | Warning | 否 | 1670-1684 |
| 12 | 擦拭塔：相对E距离 | Error | 是 | 1687-1688 |
| 13 | 擦拭塔：防渗漏 | Error | 是 | 1690-1691 |
| 14-17 | 擦拭塔：层高一致性（4项） | Error | 是 | 1719-1773 |
| 18-20 | 树状支撑：直径约束（3项） | Error | 是 | 1840-1845 |
| 21 | 无支撑时使用支撑强制器 | Warning | 否 | 1849-1861 |
| 22 | 初始层高 > 喷嘴直径 | Error | 是 | 1877-1878 |
| 23 | 层高 > 喷嘴直径 | Error | 是 | 1881-1883 |
| 24 | 挤出宽度超出范围 | Error | 是 | 1886-1897 |
| 25 | G92 E0 在 G-code 中（非 BBL） | Error | 是 | 1900-1920 |
| 26 | 耗材不匹配热床类型（`STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE`） | Error | 是 | 1925-1952 |
| 27-28 | Jerk 设置（2项） | Warning | 否 | 1980-2007 |
| 29 | Junction deviation 超出范围 | Warning | 否 | 2010-2018 |
| 30 | 加速度超出范围 | Warning | 否 | 2021-2077 |
| 31 | 墙体序列 + 精确外墙 | Warning | 否 | 2099-2102 |
| 32 | 耗材收缩补偿不匹配 | Warning | 否 | 2108-2111 |

### 云引擎独有的前处理校验

| 检查项 | 文件:行号 | 阻断？ | 说明 |
|--------|-----------|:---:|------|
| 文件格式仅 `.3mf` | `SliceEngine.cpp:314` | 是 | 桌面端通过文件对话框限制 |
| 文件大小限制（默认200MB） | `SliceEngine.cpp:326` | 是 | 桌面端无限制 |
| 后处理脚本拒绝 | `SliceEngine.cpp:414-421` | 否(warning) | 云端 RCE 防护 |
| `post_process` 配置值清除 | `SliceEngine.cpp:418` | N/A | 安全措施 |
| 打印机型号强制 `"Snapmaker U1"` | `SliceEngine.cpp:1013` | 是 | **最大差异** |
| 打印机预设应用校验（`printable_area`/`height` 非默认值） | `SliceEngine.cpp:700-718` | 是 | 验证 resources/ 目录完整性 |
| 耗材官方祖先校验 | `PresetManager.cpp:356-596` | 条件 | 强制模式下阻断 |
| 几何缺陷检查（7项） | `GeometryCheck.cpp` | 否(warning) | 桌面端无等效检查 |
| 构建体积部分超出 | `SliceEngine.cpp:1565-1571` | 是 | 与桌面端等效 |
| Plate 不存在 | `SliceEngine.cpp:1216-1238` | 是 | 云端特有 |
| 擦拭塔自动重试 | `SliceEngine.cpp` 重试逻辑 | N/A | 平台特定 CGAL 问题 |

### 关键差异

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 打印机限制 | Snapmaker U1 独占 | 任何打印机 |
| 几何预检查 | 切片前 7 项几何检查 | 无等效检查 |
| 后处理脚本 | 拒绝（RCE 防护） | 允许 |
| 耗材校验 | 必须为官方或解析到官方祖先；自动替换 | 任何耗材预设 |
| `Print::validate()` | 相同（共享 `libslic3r` 代码） | 相同 |
| 构建体积检查 | 每板 + 板局部坐标转换 | `check_volumes_outside_state()` on Canvas3D |
| 错误传递 | 结构化 JSON + 错误码 | wxWidgets 通知和 GUI 弹窗 |
| 空 Plate 处理 | `EMPTY_PLATE` 警告，跳过 | 桌面端不会创建空 plate |

---

## 5. 后处理校验（桌面端一致性）

### 架构差异

- **云引擎**：`run_postprocessing()` → `build_statistics()` → `exit_code()` 离散管道
- **桌面端**：解耦检查 — `PartPlate::is_slice_result_ready_for_print()` + `GLCanvas3D` 通知 + `MainFrame` 按钮启用/禁用

### 逐项对比

#### A. Toolpath Outside（路径超出打印区域）

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 严重级别 | ERROR | 阻塞式 |
| 是否阻断 | 是（退出码 7） | 是（禁用打印按钮） |
| 是否允许导出 | 否 | **是**（`is_slice_result_ready_for_export()` 不检查 toolpath_outside） |
| 文件 | `SliceEngine.cpp:2320`、`PlateProcessor.cpp:974` | `PartPlate.hpp:429-434` |

**一致性**：✅ 一致。两者均阻断打印。

**注意**：桌面端允许导出（即使有 toolpath_outside），但阻止打印。云引擎将两者都阻止。

#### B. Toolpath Conflict（路径冲突）

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 严重级别 | WARNING | 非阻塞警告 |
| 是否阻断 | 否 | 否（`// gcode conflict can also print`） |
| 文件 | `SliceEngine.cpp:2336`、`PlateProcessor.cpp:990` | `PartPlate.hpp:433`（注释掉） |

**一致性**：✅ 一致。云引擎特意匹配桌面端的"冲突也可打印"行为。

#### C. Bed/Filament Compatibility（热床/耗材兼容性）

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 严重级别 | WARNING | 已计算但在 UI 中无操作 |
| 是否阻断 | 否 | 否 |
| 文件 | `SliceEngine.cpp:2353`、`PlateProcessor.cpp:1007` | `GCode.cpp:1875`（仅在导出时设置） |

**一致性**：⚠️ **轻微不一致**。云引擎主动报告为警告。桌面端计算该值但没有针对它的 UI 阻断操作。

#### D. Timelapse Warnings（延时摄影警告）

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 严重级别 | WARNING | INFO 级别通知 |
| 是否阻断 | 否 | 否 |
| 检查项 | 螺旋花瓶、按对象打印 | 相同 |

**一致性**：✅ 一致。

#### E. GCode Warning Levels（GCode 警告级别）

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| Level 0 | TIP | TIP/通知 |
| Level 1-2 | WARNING（不阻断） | 通知（`NON_CRITICAL`） |
| Level 3+ | ERROR（阻断） | **不存在**（桌面端始终创建 level=1 的警告） |
| `BED_TEMP_WARNING_CODE`("1000C001") | 跳过 | 显示为通知 |

**一致性**：⚠️ **不一致**。桌面端 `GCodeProcessor::SliceWarning.level` 字段几乎未使用（始终为 1）。云引擎假定的 level>=3 边界在桌面端代码中不存在。如果桌面端将来生成 level>=3 的警告，云引擎会将其视为致命错误，而桌面端不会。

### 结果聚合差异

**云引擎**：
- 遍历所有 plate_result
- 任何 `issue.level == "error"` → plate 失败
- `success = false` → `m_any_error = true` → 退出码非零
- 使用 `set_error_type()` 取 max 值保留最严重错误

**桌面端**：
- 二进制 `is_slice_result_ready_for_print()`：仅检查 `toolpath_outside` 是否为 false
- 不聚合冲突、热床匹配、延时或 GCode 警告
- 打印和导出的就绪状态分离

---

## 总结表：云引擎 vs 桌面端一致性总览

| 校验项 | 云引擎 | 桌面端 | 一致性 |
|--------|--------|--------|:---:|
| 打印机型号限制 | 硬阻断（仅 Snapmaker U1） | 无阻断 | ❌ 不一致 |
| 打印机预设替换 | 全量覆盖 | 选择性合并 (different_settings) | ❌ 不一致 |
| 耗材预设替换 | 全量替换 + PresetRollback | 无替换 | ❌ 不一致 |
| `Print::validate()` | 共享代码 | 共享代码 | ✅ 一致 |
| 构建体积 | 每板局部坐标 | Canvas3D 全局坐标 | ✅ 等效 |
| 几何预检查 | 7 项 | 无 | ⚠️ 云引擎独有 |
| 后处理脚本 | 拒绝 | 允许 | ⚠️ 安全差异 |
| Toolpath Outside | ERROR 阻断 | 阻断打印 | ✅ 一致 |
| Toolpath Conflict | WARNING 不阻断 | WARNING 不阻断 | ✅ 一致 |
| Bed/Filament Mismatch | WARNING | 已计算无操作 | ⚠️ 轻微不一致 |
| Timelapse Warnings | WARNING | INFO | ✅ 一致 |
| GCode Warning Level≥3 | ERROR 阻断 | 不存在此级别 | ⚠️ 不一致 |
| GCode Warning Level 1-2 | WARNING | 通知 | ✅ 一致 |
| 退出码机制 | 数值退出码（0-7） | 二进制就绪状态 | N/A（不同机制） |

## 关键文件索引

### 云引擎
| 文件 | 用途 |
|------|------|
| `src/SliceEngine.cpp` | 主管道编排，所有验证阶段 |
| `src/SliceEngine.hpp` | SliceEngine 类声明，PlateSliceResult 结构体 |
| `src/PlateProcessor.cpp` | 每板处理 |
| `src/PresetManager.cpp` | 打印机/耗材预设替换和校验 |
| `src/PresetRollback.cpp` | 耗材回退到基础预设 |
| `src/GeometryCheck.cpp` | 几何缺陷检查 |
| `src/Issue.hpp` | Issue 结构体和工厂函数 |
| `src/ExitCodes.hpp` | 退出码定义 |
| `src/StatisticsBuilder.cpp` | 统计构建和退出码推导 |
| `src/EngineContext.hpp` | 共享可变状态 |

### 桌面端 OrcaSlicer (`/home/joyx/Desktop/code/OrcaSlicer/`)
| 文件 | 用途 |
|------|------|
| `src/libslic3r/Print.cpp:1548-2113` | `Print::validate()` — 共享的核心验证函数 |
| `src/libslic3r/PrintBase.hpp:19-37` | `StringExceptionType` 枚举 |
| `src/libslic3r/PresetBundle.cpp:1091-1164` | `validate_presets()` |
| `src/libslic3r/PresetBundle.cpp:2603-2841` | `load_config_file_config()`（桌面端预设选择） |
| `src/libslic3r/Preset.cpp:1933-1961` | `validate_preset()` 存在性检查 |
| `src/libslic3r/Preset.cpp:2029-2058` | `load_external_preset()` 选择性恢复 |
| `src/libslic3r/PrintConfig.cpp:7597` | `DynamicPrintConfig::validate()` |
| `src/slic3r/GUI/PartPlate.hpp:429-441` | `is_slice_result_ready_for_print()` / `_for_export()` |
| `src/slic3r/GUI/Plater.cpp:11670-11778` | `update_background_process()` |
| `src/slic3r/GUI/Plater.cpp:10200-10420` | 3MF 加载流程 |
| `src/slic3r/GUI/MainFrame.cpp:1487-1512` | `can_print()` / `can_export_all_gcode()` |
| `src/slic3r/GUI/Tab.cpp:2740-2793` | Snapmaker U1 UI 定制 |
| `src/libslic3r/GCode/GCodeProcessor.hpp:187-192` | `SliceWarning` 结构体 |
