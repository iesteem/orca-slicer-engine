# orca-slice-engine 变更综合分析报告

> 版本: v02.02.01 | 日期: 2026-06-27
> 基准: `514381a` → `791de83` (master HEAD)
> 涵盖: 70+ commits | 打印机型号、预设替换、前/后处理校验、15 bug 修复

---

## 一、退出码体系（v02.02.01 最终状态）

**Commit `0a969dc`**：按切片前/后严格归类，激活死常量。

**文件**: `src/ExitCodes.hpp`

```cpp
constexpr int EXIT_OK                  = 0;  // 成功
constexpr int EXIT_INVALID_ARGS        = 1;  // 命令行参数无效
constexpr int EXIT_FILE_NOT_FOUND      = 2;  // 输入文件未找到
constexpr int EXIT_LOAD_ERROR          = 3;  // 3MF 加载/解析失败
constexpr int EXIT_SLICING_ERROR       = 4;  // 切片引擎异常
constexpr int EXIT_EXPORT_ERROR        = 5;  // GCode 导出失败
constexpr int EXIT_PREPROCESS_ERROR    = 6;  // 切片前校验错误（配置/输入/预设不合法）
constexpr int EXIT_POSTPROCESS_ERROR   = 7;  // 切片后致命错误（GCode已生成但内容不可用）
```

**C API** (`src/slic3r_c_api.h`):

```c
#define SLIC3R_ERR_PREPROCESS      6   // 切片前校验错误
#define SLIC3R_ERR_POSTPROCESS     7   // 切片后致命错误
#define SLIC3R_ERR_INTERNAL        99  // C API 内部异常
```

### exit_code() 优先级逻辑

```cpp
// src/StatisticsBuilder.cpp:238-248
int StatisticsBuilder::exit_code() const {
    if (m_ctx.error_type > EXIT_OK)     // 显式错误码 (1-7) → 直接返回
        return m_ctx.error_type;
    if (m_ctx.any_error)                // 有错误但无明确码 → 兜底返回 6
        return EXIT_PREPROCESS_ERROR;
    if (m_ctx.any_postprocess_warning)  // 仅有非致命后处理警告 → 成功
        return EXIT_OK;
    return EXIT_OK;
}
```

`set_error_type()` 取 max 值（`if (code > m_ctx.error_type) m_ctx.error_type = code`），故 7 优先级最高。

---

## 二、打印机型号校验

### 2.1 当前实现

**文件**: `src/PresetManager.cpp:598-625`、`src/SliceEngine.cpp:1011-1038`

硬编码白名单，切片前阻断：

```
printer_model 存在？
  ├─ 否 → PRINTER_MODEL_MISSING error, EXIT_PREPROCESS_ERROR(6), 阻断切片
  └─ 是 → 值 == "Snapmaker U1"（严格相等）？
           ├─ 是 → 通过
           └─ 否 → PRINTER_MODEL_UNSUPPORTED error, EXIT_PREPROCESS_ERROR(6), 阻断切片
```

### 2.2 桌面端行为

桌面端无等效硬阻断。Snapmaker U1 检测仅用于 UI 定制（大小写不敏感子串 `boost::icontains`），从不阻断切片。文件：`Plater.cpp:10431-10453`、`Tab.cpp:2740-2793`。

### 2.3 差异说明

云引擎是 Snapmaker U1 专用切片服务，强制执行打印机型号白名单是设计意图。桌面端是多供应商系统，使用 `compatible_printers` 机制匹配预设。

---

## 三、打印机预设替换（全量覆盖）

### 3.1 管线顺序

`SliceEngine::run()` (`src/SliceEngine.cpp:186-226`) 中的配置分层：

```
1. FullPrintConfig::defaults()              ← 通用默认基线
2. load_3mf()                               ← 3MF 项目配置 + 嵌入预设
3. sanitize_config() / validate_config()    ← 值范围清理
4. load_system_presets() / validate_presets() ← 加载 Snapmaker + OrcaFilamentLibrary
5. validate_printer_model()                 ← 强制 printer_model == "Snapmaker U1"
6. apply_printer_official_preset()          ← ★ 全量覆盖
7. validate_filament_official()             ← 耗材官方祖先校验 + 替换
8. build_full_print_config()                ← 构建最终打印配置
```

### 3.2 覆盖函数

**文件**: `src/PresetManager.cpp:37-75`

两个覆盖函数，在 `substitute_printer_params()` 和 `apply_printer_official_preset()` 中均被调用：

```cpp
// (1) 全量覆盖：src 中每个键无条件写入 dst
//     标量键 → dst_opt->set(src_opt)        ← 包含 G-code 键
//     向量键 → 逐索引 set_at()
inline void overwrite_all_keys_from(DynamicPrintConfig& dst, const DynamicPrintConfig& src);

// (2) G-code 冗余安全网：显式覆盖 9 个 G-code 键
//     Bambu 专用 G-code 变量不被 Snapmaker PlaceholderParser 识别
constexpr const char* GCODE_KEYS[] = {
    "machine_start_gcode", "machine_end_gcode",
    "before_layer_change_gcode", "layer_change_gcode",
    "change_filament_gcode", "machine_pause_gcode",
    "template_custom_gcode", "printing_by_object_gcode",
    "time_lapse_gcode",
};
inline void overwrite_gcode_keys_from(DynamicPrintConfig& dst, const DynamicPrintConfig& src);
```

**注意**: `overwrite_all_keys_from()` 已处理标量键（含 G-code），`overwrite_gcode_keys_from()` 是冗余安全网（Commit `0a969dc` 重新添加，`791de83` 修复 const-correctness）。

### 3.3 apply_printer_official_preset()

**文件**: `src/PresetManager.cpp:671-778`

```
1. 从 nozzle_diameter[0] 推导官方预设名："Snapmaker U1 (0.4 nozzle)"
2. 从磁盘加载 JSON：resources/profiles/Snapmaker/machine/Snapmaker U1 (0.4 nozzle).json
3. overwrite_all_keys_from(m_config, official_cfg)
4. overwrite_gcode_keys_from(m_config, official_cfg)
5. printer_settings_id 替换为官方预设名
6. printer_model 替换为官方值
7. 验证 printable_area/printable_height 不再为默认值 → PRINTER_PRESET_NOT_APPLIED
```

### 3.4 G-code 警告消息修复（Commit `0a969dc`）

```diff
- ") — retained as-is (desktop parity)"
+ ") — will be replaced with official G-code for cloud safety"
```

旧消息声称 "retained as-is" 但后续 `apply_printer_official_preset()` 无条件覆盖了 G-code。消息已修正为准确描述实际行为。

### 3.5 桌面端行为

桌面端使用**选择性合并**（`PresetBundle.cpp:2603-2841`、`Preset.cpp:2029-2058`）：基于 `different_settings_to_system` 决定保留还是恢复为系统值。修改后的 G-code 显示对话框但**不覆盖**。

### 3.6 差异说明

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 覆盖策略 | 全量强制覆盖 | 基于 different_settings 的选择性合并 |
| G-code 处理 | 始终用官方 G-code 覆盖 | 如果被标记为已修改则保留 |
| 原因 | 云端安全：Bambu G-code 变量不兼容 | 用户可控的桌面环境 |

---

## 四、耗材预设替换（全量替换）

### 4.1 核心函数

**文件**: `src/PresetManager.cpp:356-596`（`validate_filament_official()`）、`src/PresetRollback.cpp:1-213`

Commit `1a53b71` 引入 PresetRollback，实现双路径替换。

### 4.2 完整决策流程

```
filament_settings_id[i]                    ← 3MF 中的耗材预设名
         │
         ▼
   find_in_system(name)                    ← 在 Snapmaker + OrcaFilamentLibrary 中查找
         │
    ┌────┴────────────────────────────┐
    │ 找到                            │ 未找到
    │ is_official_preset()?           │ find_in_project(name)
    │  官方 → skip                    │   ┌─ 找到 → current = 项目预设
    │  非官方 → current = 系统预设    │   └─ 未找到 → current = nullptr
    └────────┬────────────────────────┘
             │
    ┌────────┴──────────────────────────────────────────┐
    │ current == nullptr?                               │
    │                                                   │
    │  YES → 预设名在任何地方都不存在                      │
    │    enforce=false → has_inline_filament_config()?   │
    │      有 → FILAMENT_CUSTOM_INLINE warning           │
    │      无 → PresetRollback (调用点1，行 435)          │
    │    enforce=true  → PresetRollback (调用点1)         │
    │      rollback 成功 → 用基础耗材替换                  │
    │      rollback 失败 → FILAMENT_NO_OFFICIAL_ANCESTOR  │
    │                      terminal error, EXIT_PREPROCESS_ERROR(6) │
    │                                                   │
    │  NO → 预设存在但非官方                              │
    │    enforce=false → 沿 inherits 链做校验(warning)   │
    │    enforce=true  → 进入 Path 1+2 替换              │
    └──────┬────────────────────────────────────────────┘
           │ enforce=true, current != nullptr
           ▼
    ┌──────────────────────────────────────────────────┐
    │              Path 1: inherits 链查找              │
    │                                                  │
    │  while (current && !resolved) {                  │
    │      inherits_name = current->inherits()         │
    │      if (inherits_name.empty()) break;  ← 链到头 │
    │      if (循环引用) error, break;                 │
    │                                                  │
    │      parent = find_ancestor(inherits_name)       │
    │      if (!parent) break;  ← 祖先找不到            │
    │                                                  │
    │      if (is_official(parent))                    │
    │          → substitute_filament_params()          │
    │          → resolved = true, done                 │
    │      if (parent 不是耗材类型)                     │
    │          → FILAMENT_UNKNOWN_ANCESTOR error       │
    │      else current = parent;  ← 继续向上走        │
    │  }                                               │
    └──────────────────┬───────────────────────────────┘
                       │ resolved == false
                       ▼
    ┌──────────────────────────────────────────────────┐
    │         Path 2: PresetRollback 兜底 (调用点2)     │
    │                 行 537                           │
    │                                                  │
    │  不依赖 inherits 链。从 m_config 读取：           │
    │    filament_type[i]    = "PLA"                   │
    │    filament_vendor[i]  = "Snapmaker"             │
    │                                                  │
    │  Pass 1: 同供应商匹配 (find_in_vendor)           │
    │    筛选：vendor 匹配 && filament_type 匹配        │
    │    评分：含"Base" +100, 非实例化 +80,             │
    │          inherits fdm_filament_* +60,            │
    │          名称无喷嘴直径 +40                       │
    │                                                  │
    │  Pass 2: 通用匹配 (find_generic)                 │
    │    筛选：不限 vendor                              │
    │    评分：OrcaFilamentLibrary +100,               │
    │          含"Generic" +80, 非实例化 +60,          │
    │          inherits 为空(root) +50,                │
    │          inherits fdm_filament_common +40        │
    │                                                  │
    │  rollback 成功 → 用基础耗材全量替换               │
    │  rollback 失败 → FILAMENT_NO_OFFICIAL_ANCESTOR   │
    │                  terminal error, EXIT_PREPROCESS_ERROR(6) │
    └──────────────────────────────────────────────────┘
```

### 4.3 关键设计规则

1. **inherits 链优先，PresetRollback 永远是最后方案**
2. **PresetRollback 两个调用点**：调用点1（行 435，`current==nullptr`，唯一方案）；调用点2（行 537，inherits 链耗尽，兜底方案）
3. **`inherits` 字段来源**：预设对象的元数据，从 3MF 嵌入预设的 `inherits` 字段读入
4. **`substitute_filament_params()`**（行 564-596）：无条件全量覆盖该挤出机的所有向量键值，`filament_settings_id[i]` 替换为官方预设名

### 4.4 替换策略演变

| Commit | 变更 |
|--------|------|
| `f2d6356` | 属性自动匹配回退（PresetRollback 前身） |
| `1a53b71` | PresetRollback 正式引入，双路径替换 |
| `67969ee` | `substitute_filament_params()` 从 nil-fill 改为全量覆盖 |

### 4.5 成功率影响

**不会降低切片成功率**：
- 继承链完整性已被保证（`ffaa09c`/`9336670` 修复 circular inherits）
- 祖先缺失有降级路径（`f2d6356` 验证：10 盘全部成功，此前 0 盘）
- 替换后参数是官方值，不会引入非法值
- 配置分层确保 3MF 值最高优先级（`build_full_print_config()` 合并顺序：defaults → 系统 printer → 系统 filament → m_config）

### 4.6 桌面端行为

桌面端**无耗材替换机制**。`validate_preset()` 仅做存在性检查（warning）。`is_filament_compatible()` 仅用于多材料兼容性检查。

---

## 五、前处理校验

### 5.1 共享代码：Print::validate()

云引擎和桌面端使用**完全相同的** `Print::validate()` 实现（来自 `libslic3r` 静态库，`Print.cpp:1548-2113`）。

#### 完整检查项（32 项）

| # | 检查项 | 严重级别 | 阻断？ |
|---|--------|----------|:---:|
| 1 | 无对象 | Error | 是 |
| 2 | 无挤出机 | Error | 是 |
| 3 | 多耗材温度不匹配 | Error | 是 |
| 4 | 延时摄影 + 按对象 | Error | 是 |
| 5 | 顺序打印碰撞 | Error | 是 |
| 6 | 层打印碰撞 | Warning | 否 |
| 7 | 螺旋花瓶多副本 | Error | 是 |
| 8 | 螺旋花瓶多材料 | Error | 是 |
| 9 | 超出构建体积高度 | Error | 是 |
| 10 | 可变层高 + 树状支撑 | Error | 是 |
| 11 | 擦拭塔：不同喷嘴/耗材直径 | Warning | 否 |
| 12 | 擦拭塔：相对E距离 | Error | 是 |
| 13 | 擦拭塔：防渗漏 | Error | 是 |
| 14-17 | 擦拭塔：层高一致性（4项） | Error | 是 |
| 18-20 | 树状支撑：直径约束（3项） | Error | 是 |
| 21 | 无支撑时使用支撑强制器 | Warning | 否 |
| 22 | 初始层高 > 喷嘴直径 | Error | 是 |
| 23 | 层高 > 喷嘴直径 | Error | 是 |
| 24 | 挤出宽度超出范围 | Error | 是 |
| 25 | G92 E0 在 G-code 中（非 BBL） | Error | 是 |
| 26 | 耗材不匹配热床类型 | Error | 是 |
| 27-28 | Jerk 设置（2项） | Warning | 否 |
| 29 | Junction deviation 超出范围 | Warning | 否 |
| 30 | 加速度超出范围 | Warning | 否 |
| 31 | 墙体序列 + 精确外墙 | Warning | 否 |
| 32 | 耗材收缩补偿不匹配 | Warning | 否 |

#### STRING_EXCEPT 错误码映射

云引擎在 `SliceEngine.cpp:2034` 和 `PlateProcessor.cpp:940` 中将 `StringExceptionType` 映射为 issue code：

| StringExceptionType | Issue Code |
|---|---|
| `STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE` | `PRINT_VALIDATE_FILAMENT_BED_MISMATCH` |
| `STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP` | `PRINT_VALIDATE_FILAMENT_TEMP_MISMATCH` |
| `STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT` | `PRINT_VALIDATE_OBJECT_COLLISION_SEQ` |
| `STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT` | `PRINT_VALIDATE_OBJECT_COLLISION_LAYER` |
| `STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT` | `PRINT_VALIDATE_LAYER_HEIGHT_LIMIT` |
| Default | `PRINT_VALIDATE_ERROR` |

### 5.2 云引擎独有检查

| 检查项 | 文件:行号 | 阻断？ | 退出码 | 说明 |
|--------|-----------|:---:|--------|------|
| 文件格式 `.3mf` | `SliceEngine.cpp:314` | 是 | 3 | 桌面端通过文件对话框限制 |
| 文件大小（默认200MB） | `SliceEngine.cpp:326` | 是 | 3 | 云端资源管控 |
| 后处理脚本拒绝 | `SliceEngine.cpp:414-421` | 否 | — | 云端 RCE 防护 |
| 打印机型号白名单 | `SliceEngine.cpp:1013` | 是 | 6 | 单打印机网关 |
| 打印机预设应用校验 | `SliceEngine.cpp:700-718` | 是 | 6 | 验证 printable_area/height 非默认值 |
| 耗材官方祖先校验 | `PresetManager.cpp:356-596` | 条件 | 6 | 强制模式下阻断 |
| Plate 不存在 | `SliceEngine.cpp:1486` | 是 | 6 | 单盘模式 plate_id 校验 |
| 构建体积部分超出 | `SliceEngine.cpp:1869` | 是 | 6 | 与桌面端等效 |
| 擦拭塔自动重试 | `SliceEngine.cpp` retry logic | N/A | — | 平台特定 CGAL 问题 |

### 5.3 几何缺陷检查（7 项，云引擎独有）

**文件**: `src/GeometryCheck.cpp`。Commit `1a53b71` 将所有 7 项统一为 warning，Commit `bf91bfb` 曾将 GEOM_EMPTY/GEOM_ZERO_VOLUME 短暂升级为 error。Commit `1a53b71` 恢复为全部 warning（对齐桌面端：几何问题不阻断切片）。

| 检查项 | 错误码 | 严重级别 |
|--------|--------|:---:|
| 空网格 | `GEOM_EMPTY` | Warning |
| 零体积 | `GEOM_ZERO_VOLUME` | Warning |
| 非流形 | `GEOM_NON_MANIFOLD` | Warning |
| 退化面 | `GEOM_DEGENERATE` | Warning |
| 自相交 | `GEOM_SELF_INTERSECT` | Warning |
| 多组件 | `GEOM_MULTI_COMPONENT` | Warning |
| 翻转法线 | `GEOM_INVERTED` | Warning |

所有几何检查的 `has_geom_error` 阻断分支已是死代码（Commit `0a969dc` 移除）。

### 5.4 与桌面端的关键差异

| 方面 | 云引擎 | 桌面端 |
|------|--------|--------|
| 打印机限制 | Snapmaker U1 独占 | 任何打印机 |
| 几何预检查 | 7 项（warning） | 无等效 |
| 后处理脚本 | 拒绝（RCE 防护） | 允许 |
| 耗材校验 | 必须为官方或解析到官方祖先 | 任何耗材预设 |
| `Print::validate()` | 相同（共享 libslic3r） | 相同 |
| 配置验证模式 | `under_cli=false`（GUI 宽松） | `under_cli=false` |

---

## 六、后处理校验

### 6.1 当前实现

**文件**: `src/SliceEngine.cpp:2106-2191`、`src/PlateProcessor.cpp:1012-1099`

Commit `0a969dc` 实现了与桌面端 `PartPlate::is_slice_result_ready_for_print()` 的行为对齐。

### 6.2 逐项对比

#### A. Toolpath Outside — ERROR, 阻断（一致性 ✅）

```cpp
// SliceEngine.cpp:2109-2117, PlateProcessor.cpp:1015-1023
// Desktop blocks printing via is_slice_result_ready_for_print()
// when toolpath_outside is true.
if (result.gcode_result.toolpath_outside) {
    has_postprocess_error = true;   // ← 致命错误
    result.issues.push_back(make_error(plate_id, "TOOLPATH_OUTSIDE", ...));
}
// → set_error_type(EXIT_POSTPROCESS_ERROR);  // 退出码 7
```

桌面端 `PartPlate.hpp:429-434`：`toolpath_outside` 导致 `is_slice_result_ready_for_print()` 返回 false，禁用打印按钮。

#### B. Toolpath Conflict — WARNING, 不阻断（一致性 ✅）

```cpp
// SliceEngine.cpp:2124-2126
// Desktop allows printing with conflicts (PartPlate.hpp:433:
// "gcode conflict can also print"). Match that behavior — warning only.
if (result.gcode_result.conflict_result.has_value()) {
    has_postprocess_warning = true;  // ← 仅警告
}
```

桌面端冲突检查代码被注释掉：`// && !m_gcode_result->conflict_result.has_value()  gcode conflict can also print`

#### C. Bed/Filament Mismatch — WARNING, 不阻断（轻微不一致 ⚠️）

云引擎主动报告为 `BED_FILAMENT_MISMATCH` warning。桌面端计算 `bed_match_result`（`GCode.cpp:1875`）但无对应 UI 阻断操作。

#### D. Timelapse Warnings — WARNING, 不阻断（一致性 ✅）

螺旋花瓶和按对象打印的延时摄影限制均为 warning。

#### E. PrintBase Step Warnings — 新增（Commit `0a969dc`）

```cpp
// SliceEngine.cpp:2016-2038, PlateProcessor.cpp:922-944
// Desktop CLI treats EmptyGcodeLayers and GcodeOverlap appropriately.
if (msg_type == PrintStateBase::SlicingEmptyGcodeLayers) {
    // EmptyGcodeLayers: plate has ranges with no layers → error
    make_error(plate_id, "PRINT_EMPTY_GCODE_LAYERS", ...)
} else if (msg_type == PrintStateBase::SlicingGcodeOverlap) {
    // GcodeOverlap: reserved → warning
    make_warning(plate_id, "PRINT_GCODE_OVERLAP", ...)
}
```

EmptyGcodeLayers 触发 `has_postprocess_error` → `EXIT_POSTPROCESS_ERROR`(7)，且跳过重试并丢弃空 G-code 文件。

#### F. GCode Warning Levels

| 级别 | 云引擎行为 | 桌面端行为 | 一致性 |
|:---:|-----------|-----------|:---:|
| Level 0 | TIP | TIP/通知 | ✅ |
| Level 1-2 | WARNING（不阻断） | 通知（`NON_CRITICAL`） | ✅ |
| Level 3+ | ERROR（阻断，退出码 7） | 不存在（始终 level=1） | ⚠️ |

### 6.3 与桌面端一致性总览

| 校验项 | 云引擎 | 桌面端 | 一致性 |
|--------|--------|--------|:---:|
| Toolpath Outside | ERROR(7) 阻断 | 阻断打印 | ✅ |
| Toolpath Conflict | WARNING 不阻断 | WARNING 不阻断 | ✅ |
| Bed/Filament Mismatch | WARNING | 已计算无操作 | ⚠️ |
| Timelapse Warnings | WARNING | INFO | ✅ |
| EmptyGcodeLayers | ERROR(7) 阻断 | 阻断（桌面 CLI） | ✅ |
| GCodeWarning Level 1-2 | WARNING | 通知 | ✅ |
| GCodeWarning Level 3+ | ERROR(7) | 不存在此级别 | ⚠️ |

---

## 七、Bug 修复

### 7.1 修复总览

| 类别 | Bug 数量 | 修复状态 |
|------|:---:|:---:|
| Z-baking / 坐标变换 | 3 | ✅ |
| 内存 | 3 | ✅ |
| 崩溃处理 | 3 | ✅ |
| 配置验证 | 4 | ✅ |
| 可观测性 | 2 | ✅ |
| **总计** | **15/15** | **修复前崩溃率 ~32%，修复后 ~0%** |

### 7.2 Z-baking 修复（3 bug，🔴 P0）

#### B1: z_trans 未除以 z_scale（Commit `0773324`）

- **根因**: `Print::apply()` 保留 Z_scale 但丢弃 Z translation
- **表现**: Z 有缩放的模型空层
- **修复**: `PlateProcessor.cpp:684` — `double z_adjustment = inst_offset.z() / z_scale;`

#### B2: ~90° 旋转除近零（Commit `0ec93d4`）

- **根因**: 变换矩阵 (2,2) ≈ -2.1e-06，除法放大 → ~-780 万 mm
- **表现**: G-code 坐标飞出 47km
- **修复**: `PlateProcessor.cpp:682-683` — 双重 epsilon 防护

#### B3: bbox 缓存跨盘未失效（Commit `bf91bfb`）

- **根因**: `m_min_max_z_valid` 跨盘保持 true
- **表现**: 多盘非首盘报 "No layers were detected"
- **修复**: `PlateProcessor.cpp:652,728` — Z-baking 前后双重 `invalidate_bounding_box()`

### 7.3 内存修复（3 bug，🔴 P0）

#### B4: G-code 可视化内存泄漏（Commit `ca60e13`）

- **根因**: `moves`/`lines_ends` 保留所有 G-code 顶点，每盘数百 MB
- **表现**: 多盘 → `std::bad_alloc`
- **修复**: `PlateProcessor.cpp:315-318` — 每盘后 `clear()` + `shrink_to_fit()`

#### B5: 空层 → SIGSEGV（Commit `ca60e13`）

- **根因**: "No layers were detected" 后仍调用 `GCode::_do_export()`
- **表现**: SIGSEGV 段错误
- **修复**: "No layers" 判定为 non-fatal，跳过 G-code 导出

#### B6: 静默崩溃返回空 JSON（Commit `502de67`）

- **根因**: 引擎内部 OOM/SIGSEGV/SIGABRT 无防护
- **表现**: `{"issues":[], "plates":[]}` 零诊断信息
- **修复**: `main.cpp:103-130` — POSIX 信号处理 + `sigaltstack()` + `write_emergency_json()`

### 7.4 崩溃处理修复（3 bug）

#### B7: 反向前缀匹配 → bad_alloc（P0）

- **表现**: `preset.name.size() >= name.size()` 单向前缀匹配，12/18 测试模型崩溃
- **修复**: 双向 shorter/longer 前缀匹配
- **影响**: server 默认模式崩溃率 32.2% → 0%

#### B8: 多 filament flush 参数清零（P1）

- **表现**: `flush_volumes_matrix`/`enable_prime_tower` 被无条件清零 → rc=6
- **修复**: 仅 `n_fil <= 1` 时清零

#### B9: Bambu G-code key 残留（Commit `c71010e`，P1）

- **表现**: Bambu 专用 G-code 在替换后存活 → PlaceholderParser 失败
- **修复**: `overwrite_gcode_keys_from()` 强制覆写 9 个 G-code key
- **当前状态**: `overwrite_all_keys_from()` 已覆盖所有键，`overwrite_gcode_keys_from()` 是冗余安全网

### 7.5 配置验证修复（4 bug）

#### B10: CLI 模式验证过严（Commit `fdd5c05`，P1）

- **根因**: `validate_config(under_cli=true)` → 无效配置值 fatal error
- **修复**: `under_cli=false` 匹配桌面 GUI 宽松模式

#### B11: BakedInstanceZ 恢复时机错误（P0）

- **根因**: `apply_model()` 在 `print.apply()` 后立即恢复 Z offset
- **表现**: 薄片/Z offset 模型零交点 → No layers
- **修复**: 6 个调用点分布在正常/重试/跳过路径，slicing 完成后才恢复

#### B12: plate_width 精度（Commit `85da155`，P1）

- **根因**: 多减 `BED_AXES_TIP_RADIUS` + 未 int 截断
- **表现**: 0.9mm 原点偏差 → 边缘模型被误判 BUILD_VOLUME_PARTLY_OUTSIDE
- **修复**: `static_cast<double>(static_cast<int>(bbox.size().x()))` 对齐桌面版

#### B13: 异常诊断信息丢弃（P1）

- **根因**: `StringObjectException`/`SlicingErrors` 被通用 `catch(...)` 吞掉
- **修复**: 分类型 catch，每个保留 `e.what()`

### 7.6 可观测性修复（2 bug）

#### B14: 空盘/零盘静默（P2）

- **修复**: `EMPTY_PLATE` warning + `NO_PLATES` error

#### B15: Filament 数量修剪（P0）

- **表现**: 5-6 filament → extruder ID 越界 → WipeTower 连环 SIGSEGV
- **修复**: 三级回退修剪 + 防御层保留，成功率 25/33 → 33/33

---

## 八、关键 Commit 索引

| Commit | 日期 | 描述 | 章节 |
|--------|------|------|:---:|
| `791de83` | 06-27 | const-correctness in overwrite_gcode_keys_from | 三 |
| `0a969dc` | 06-27 | 退出码统一 + 打印机覆盖 + 后处理 + EmptyGcodeLayers | 一、三、六 |
| `67969ee` | 06-27 | 打印机/耗材全量覆写重构 | 三、四 |
| `55ad4ca` | 06-27 | version bump to 02.02.01, default to consumer mode | — |
| `1a53b71` | 06-27 | PresetRollback 双路径耗材替换 | 四 |
| `bf91bfb` | — | bbox 缓存失效 + SlicingErrors 详情 + 几何严重级别 | 五、七 |
| `85da155` | — | 5 模型切片失败修复（2 P0 + 3 优化） | 七 |
| `0773324` | — | Z-baking 修复 + 预设替换简化 + 后处理对齐桌面版 | 七 |
| `f2d6356` | — | 祖先断裂时的属性自动匹配回退 | 四 |
| `88573ad` | — | 校验和后处理对齐桌面 GUI | 五、六 |
| `c71010e` | — | G-code key 强制覆写 | 三、七 |
| `fdd5c05` | — | 配置验证对齐桌面 GUI | 五、七 |
| `ca60e13` | — | 3MF 错误处理 + G-code 内存释放 | 七 |
| `0ec93d4` | — | ~90° 旋转除近零防护 | 七 |

---

## 九、关联文档

| 文档 | 关联章节 |
|------|----------|
| `docs/exit-codes-6-7-refactor.md` | 一 退出码重构方案 |
| `docs/cloud-desktop-validation-comparison.md` | 二~六 云引擎 vs 桌面端逐项对比 |
| `bug-catalog-02.01.18.md` | 七 全部 bug 列表 |
| `Z-baking根因分析与修复.md` | 七、7.2 |
| `五模型切片失败根因分析.md` | 七、7.2, 7.5 |
| `几何缺陷检查流程与阻断机制分析.md` | 五、5.3 |
| `Filament数量修剪逻辑优化.md` | 七、7.6 |
| `3MF配置覆盖关系分析.md` | 三、四 配置分层 |
| `gcode-fix-analysis.md` | 七、7.4 |
| `云引擎技术方案.md` | 架构总览 |
