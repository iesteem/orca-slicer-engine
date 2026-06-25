# aedfef6fbed1a102 模型切片失败根因分析

> 引擎版本: v02.01.16 | 日期: 2026-06-25

---

## 一、现象

模型 `aedfef6fbed1a102.3mf`（Buffalo Critter Layouts）在云引擎默认模式下切片失败（exit 6），错误码 `FILAMENT_UNKNOWN_ANCESTOR`。

## 二、模型特征

| 属性 | 值 |
|------|-----|
| 打印机 | Snapmaker U1 (0.2 nozzle) |
| 耗材 | U1 PETG 0.2 nozzle JL ×1 + U1 PETG 0.2 nozzle JL(Buffalo Critter Layouts.3mf) ×3 |
| 对象 | Critter Disc v1.step 等 12 个对象 |
| Plate 数量 | 10 |
| 项目内嵌预设数 | 2 个自定义耗材预设 |

## 三、失败链路

### 唯一根因：`@System` 命名空间祖先缺失 → 继承链断裂

**位置**: `src/SliceEngine.cpp:989-996`（enforce 模式） + `src/SliceEngine.cpp:929-936`（non-enforce 模式）

#### 3.1 继承链查找失败

3MF 文件中的项目内嵌耗材预设 `U1 PETG 0.2 nozzle JL(Buffalo Critter Layouts.3mf)` 设置了：

```
inherits: "Generic PETG @System"
```

引擎在 `validate_filament_official()` 中遍历继承链时，调用 `find_ancestor("Generic PETG @System")` 查找父预设，但该预设不在 `m_preset_bundle->filaments` 中，返回 nullptr。

#### 3.2 预设为何未被加载

`OrcaFilamentLibrary.json` 的 `filament_list` 为空 `[]`。libslic3r 新版本有磁盘扫描回退（`PresetBundle.cpp:3163-3183`）自动发现 `OrcaFilamentLibrary/filament/*.json` 文件，但当前引擎链接的 libslic3r 库缺少此回退代码，导致 `Generic PETG @System.json`（文件确实存在于 `package/resources/profiles/OrcaFilamentLibrary/filament/`）从未被加载。

**文件存在但未加载的证据**：
```bash
$ ls package/resources/profiles/OrcaFilamentLibrary/filament/Generic\ PETG\ @System.json
# 文件存在，内容：
{ "name": "Generic PETG @System", "inherits": "fdm_filament_pet", ... }

$ grep "Generic PETG" package/resources/profiles/OrcaFilamentLibrary.json
# 输出为空 — vendor index 中无此条目
```

#### 3.3 无恢复路径

在 enforce 模式 while 循环中（line 958-1021），属性自动匹配（`match_inline_to_official_preset`）仅在 `inherits` 为空时触发（line 960）。当 `inherits` 非空但父预设找不到时，代码直接报错 `FILAMENT_UNKNOWN_ANCESTOR`，**无任何恢复路径**。

同样，non-enforce 模式的 `walk_chain`（line 918-941）遇到未知祖先也直接报 `make_error`，且设置 `any_error = true`，导致验证整体失败。

#### 3.4 完整调用链

```
SliceEngine::run()
  → validate_presets()
    → validate_filament_official(enforce=true)
      → find_in_project("U1 PETG 0.2 nozzle JL(Buffalo Critter Layouts.3mf)") → 找到项目内嵌预设
      → enforce while 循环:
          inherits_name = current->inherits()           // = "Generic PETG @System"
          inherits_name.empty()?                        // = false, 跳过 auto-match 路径
          parent = find_ancestor("Generic PETG @System") // → nullptr!
          → FILAMENT_UNKNOWN_ANCESTOR error             // 直接阻断
```

## 四、修复方案

### 策略：祖先缺失时回退到属性自动匹配

在继承链查找失败时，不立即报错，而是利用项目内嵌预设自身的材料属性（filament_type、喷嘴温度、热床温度）通过 `match_inline_to_official_preset` 找到最匹配的官方耗材。这与 orphaned preset（inherits 为空）已有的恢复路径（line 960-978）完全一致。

**选择属性匹配而非命名空间映射的原因**：
- 属性匹配基于实际材料参数，不依赖命名约定（`@System`、`@base`、`@vendor` 等）
- 覆盖所有祖先缺失场景，无论原因（命名空间差异、vendor index 缺失、文件损坏）
- 三层过滤（filament_type → 喷嘴温度 → 热床温度）比"猜名字"更精准

### 具体修改

| # | 位置 | 修改 | 说明 |
|---|------|------|------|
| 1 | `src/SliceEngine.cpp:~1008` enforce while 循环 | `find_ancestor` 返回 null 时，先调用 `match_inline_to_official_preset(i)`（m_config），再回退到 `match_inline_to_official_preset(&current->config, 0)`，匹配成功则 substitute | 双路径回退：优先使用项目级 per-extruder 数据，确保 filament_type 不被 default 覆盖 |
| 2 | `src/SliceEngine.cpp:~929` walk_chain | `FILAMENT_UNKNOWN_ANCESTOR` 从 `make_error` 改为 `make_warning`，不设置 `any_error` | non-enforce 模式允许自定义耗材，未知祖先不阻断切片 |
| 3 | `src/SliceEngine.cpp:~948` non-enforce 调用处 | `walk_chain` 失败后，同样的双路径 auto-match 回退 | 与 enforce 模式一致的恢复逻辑 |

### 关键设计点

**m_config 优先于 preset config**：项目级配置 `m_config` 包含 3MF `project_settings.config` 的 per-extruder 数据，其 `filament_type` 已被正确设置（如 "PETG"）。而项目内嵌预设的 `current->config` 在 `load_project_embedded_presets` 中因父预设缺失，从 `default_preset.config` 继承了错误的默认值（`filament_type="PLA"`）。

先用 `match_inline_to_official_preset(i)`（读 m_config），失败再回退到 `match_inline_to_official_preset(&current->config, 0)`，确保最佳匹配质量。

### 修复后结果

```
exit 6（模型几何问题，非耗材验证）
FILAMENT_UNKNOWN_ANCESTOR: 0 个
auto-matched: U1 PETG 0.2 nozzle JL → Generic PETG ✓
auto-matched: U1 PETG 0.2 nozzle JL(Buffalo Critter Layouts.3mf) ×3 → Generic PETG ✓
gcode: 4 个 .3mf 文件（~5MB each）
print_time: 03:06:13
total_weight: 19.63g
plates: 10 个全部切片成功
```

> 注：exit 6 来自 Plate 7-8 上 `Critter Disc v1.step` 的空层几何体问题（`PRINT_WARNING: Object can't be printed for empty layer between X and Y`），与本次耗材修复无关。该问题在桌面版同样存在。

## 五、影响范围

- 修复 1 影响所有项目内嵌耗材预设继承链断裂的场景（enforce 模式，云端默认）
- 修复 2+3 影响 non-enforce 模式（`--allow-custom-filament-presets`）
- 属性自动匹配复用已有函数（`match_inline_to_official_preset` + `substitute_filament_params`），不改变匹配逻辑本身
- 不改变其他继承链错误（FILAMENT_CIRCULAR_INHERITS、FILAMENT_NO_OFFICIAL_ANCESTOR）的处理
