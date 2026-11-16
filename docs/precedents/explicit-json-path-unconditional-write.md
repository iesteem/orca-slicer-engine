# P-28: 显式 json_output_path 无条件写文件（2026-10-13 防垃圾文件决策收窄）

**状态：** 已修复（2026-10-19，同日完成契约文档同步）
**触发场景：** `BD-01__喷嘴直径越界检测=0.3mf`（喷嘴直径=0）→
`apply_preset_substitution()` 找不到 `"Snapmaker U1 (0 nozzle)"` → rc=6，
CLI 未生成 JSON 统计文件——即使调用方明确要了 JSON 路径。

## 现象

CLI 失败时用户拿不到 JSON 报告文件，只能靠 `-v` 看 stats dump。
云服务走 C API `stats_out` 不受影响，但文件通道对显式路径静默失约。

## 根因

C API 写文件守卫（src/slic3r_c_api.cpp，2026-10-13 引入）：

```cpp
if (!engine.output_path().empty())   // 外层守卫包住了路径解析 + 写入
```

- rc=6 失败发生在 `resolve_output_path()`（run() 成功路径内才会调）之前
  → `engine.output_path()` 恒空 → **连显式 `cfg.json_output_path` 分支也被跳过**。
- 该守卫的本意只是防 auto-derive 分支从空路径推导出 CWD 下字面 `.json`
  垃圾文件（ctest build-dir CWD 曾掩盖该问题）——**防错了面**：显式路径
  分支（`ensure_extension`）从空串仍得空串，天然无垃圾文件风险。
- 伴生缺陷：CLI 裸 `-j`（不带值）发空串路径 → `ensure_extension("")`
  仍空 → 只打印 dump 不落盘，与帮助文本 "Default: auto-derived" 矛盾。

## 修复（宽语义，用户 2026-10-19 批准）

1. **守卫重构**：先解析路径再判空写——
   `cfg.json_output_path` 非空 → `ensure_extension` 后**无条件写**（含
   早期失败 rc=3/6）；否则 `engine.output_path()` 非空 → auto-derive 写；
   两者皆空（未传路径的 C API 直调方 + 失败运行）→ 跳过，2026-10-13
   决策收窄**仅**保住这一场景。
2. **裸 `-j` 对齐**：main.c 裸 `-j` 不再发空串，保持 NULL 落入
   `<output>.json` 预填（与不传 `-j` 一致）。

### 语义边界（不可漂移）

- **宽语义**：CLI 因 main.c 恒预填路径，任何**引擎执行过**的失败
  （rc=3/4/5/6/7）都落盘 JSON——与 `.log` 行为对齐（log 本就恒写）。
- rc=1（参数错，main.c 侧返回，引擎未建）与 rc=99（C API catch 路径，
  写文件在 try 内）**仍无文件**——"无文件=无报告"仅剩这两码加"未传
  路径的直调方失败"。
- 不变式不动：`success ⟺ rc==0 ⟺ error_message==""`（该字段 2026-10-19
  同日改名 `error_code` 并改为 error/serious_warning issue code 聚合，
  见 json-output.md 变更表；不变式形式不变）；早期失败报告中
  `print_info_total.output_file` 为 `""`（契约本就如此，非本次变更）。
- 成功路径与 OUTPUT_PATH_RELOCATED 场景零变化（CLI 显式分支照旧落
  请求基名）。

## 验证证据

- `tests/test_c_api.cpp` 新增
  `C API: explicit json_output_path written on early failure (P-28)`：
  空资源目录 PRESETS_MISSING + 显式 `json_output_path` → rc==6、文件
  存在、`exit_code==6`、`error_message` 与 `slic3r_get_error` 同串
  （断言后随 2026-10-19 `error_code` 改名更新为
  `error_code=="PRESETS_MISSING"`）、
  `output_file==""`。P-09 用例（不传 key）继续断言**无**文件——两用例
  合钉"有 key 写、无 key 不写"。
- `tests/test_cli.cpp` 新增
  `CLI: json stats file written on early failure; bare -j pre-fills (P-28)`：
  显式 `-j` 与裸 `-j` 两条变体均断言 rc==6 且文件落盘 `exit_code==6`。
- 端到端：BD-01 fixture（喷嘴=0）实测 `-o /tmp/bd01` → `/tmp/bd01.json`
  落盘、`exit_code==6`。

## 已证伪 / 已否决的方案

- **窄语义**（仅用户键入 `-j <path>` 才写，CLI 预填不算）：需要 main.c
  不预填、交 C API 成功时 auto-derive——副作用是 OUTPUT_PATH_RELOCATED
  换名场景下成功路径 JSON 会落换名后路径而非请求路径，且"显式 vs 预填"
  在 params_json 层不可区分，实现绕。用户选宽语义。
- **main.c 失败时自行把 stats buffer 写文件**：stats buffer 32768 上限
  会截断大报告（20 盘模型），且 C API 消费方拿不到该行为。否决。
