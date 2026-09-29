# P-09: --skip-preset-substitution + system presets 加载失败 → 必崩（已修复）

**状态：** 根因已 gdb 确认（2026-08-06），**已修复（feature-2.2.0）**：
run() 在 load_system_presets() 之后、skip 分支之前统一做
`!m_presets_available || !m_preset_bundle` 硬检查，缺失即 exit 6
PRESETS_MISSING（skip 只跳过官方预设强制替换，不跳过加载失败硬检查）。
回归测试见 tests/test_c_api.cpp（P-09 case）。

**后续（2026-09-29）：** `--skip-preset-substitution` 参数已整体移除——
P-09 修复后它只剩"跳过官方预设替换 + 跳过用户内容剥离"，与云切片
合规约束冲突且生产从不使用。CLI flag 与 JSON 键均静默忽略（未知
参数/未知键不报错），旧调用方无害。回归测试转为"废弃 JSON 键静默
忽略 + PRESETS_MISSING 干净失败"。本文保留作崩溃链历史记录。

## 现象

资源目录里任一 vendor 包损坏（如 MagicMaker/filament 缺
`fdm_filament_common.json`，而 `fdm_filament_peek.json` 声明
`inherits: fdm_filament_common` 同目录找不到）→ `load_system_presets`
整体失败。此时：

- 默认调用：exit 6 `PRESETS_MISSING`，干净失败 ✅
- `--skip-preset-substitution`：**exit 139 SIGSEGV** ❌（任何模型必崩）

## 崩溃链

skip-substitution 让流程不因 PRESETS_MISSING 退出 → `init_print`（
SliceEngine.cpp:2124）调 `is_bbl_vendor()` → `get_current_vendor_type()`
（PresetBundle.cpp:530）→ `&printers.get_edited_preset().config` 半初始化
→ :534 `config->opt_string("printer_model")` 解引用 null。

## 修复方向

- `init_print` 前检查 system presets 是否真加载成功；
  `--skip-preset-substitution` 应只跳过"官方预设强制替换"，**不应跳过
  "预设加载失败"的硬性前置检查**。
- 或上游 `get_current_vendor_type()` 加 null 防御。

## 附带发现

任何单个 vendor 包损坏会拖垮**全部** system presets 加载（inherits 是同
vendor 目录相对解析）。补拷 common 文件不够——上游对 common 基类注册有
额外要求。
