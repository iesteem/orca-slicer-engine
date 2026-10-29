# P-36: project_settings BBL 特殊键数组化 → 整配置弃载 → 误报 "Printer model is missing"（未修复）

**状态：** 未修复（2026-10-28 记录判例；引擎侧修复方案待批）
**fixture：** `tests/fixtures/bc053c0feb92b10c.3mf`（生产原件，4.6M，
md5 `c93791fc498cc0ac28d03d6c5f50cbed`，2026-10-28 从回收站救回入库——
当日 18:22 用户清理 Downloads 时原件已入回收站）

## 现象（bc053c0 用例）

切片 rc=6：`Printer model is missing. Only the Snapmaker U1 is supported.`

**误报**——文件 `Metadata/project_settings.config` 里 `printer_model =
"Snapmaker U1"` 完好。日志先出现两条 error：

```
load_from_json: parse .../_temp_3.config got a generic exception,
  reason = [json.exception.type_error.302] type must be string, but is array
Error load config from json:std::exception: ...
```

## 根因（两层）

1. **写盘方演进**：新版桌面端存盘把 `project_settings.config` 的
   `setting_id` / `filament_id` 从标量写成**数组**（每槽一份；本例 4 槽
   4 元素同值，如 `["1195313935011"×4]`）。
2. **读侧缺数组容忍**：`ConfigBase::load_from_json`（fork
   `OrcaSlicer/src/libslic3r/Config.cpp:872-876`，引擎链接同一实现）对
   BBL 特殊键（version / is_custom / name / url / type / setting_id /
   filament_id / from / description / instantiation / inherits /
   renamed_from）直接 `key_values.emplace(key, it.value())`——nlohmann
   json 值隐式转 `std::string`，遇数组抛 `type_error.302`。对比：同函数
   else 分支的普通配置键 `is_string()` / `is_array()` 双路径齐全，
   **唯特殊键分支缺数组处理**。

异常在 `load_from_json` 顶层 catch 后**整个 project_settings 弃载** →
`m_config` 无 `printer_model` → `validate_printer_model()`
（SliceEngine.cpp:858，fail-fast 在预设加载前）误报 rc=6。

## 验证（2026-10-28 双路独立实证）

- **路径 A（判例记录者）**：`setting_id`/`filament_id` 降为数组首元素、
  其余逐字节保留重打包 → 同引擎 rc=0（1 盘 PLA 白 22.31 g / 7028 mm /
  01:17:48）。
- **路径 B（用户侧独立，同日 18:21）**：桌面端重存工程（437 键重写，
  `setting_id`/`filament_id` **整键消失**）→ 切片成功。
- 两路唯一共同点：**数组消失** → 根因链闭合。

## 已证伪的推测

- "打印机型号不支持"：`printer_model` 就在文件里，是误报。
- "3MF 结构损坏"：仅改两个键即成功。

## 绕行（QA 口径）

- 桌面端重存工程（用户已实测）；或预处理把两键降为数组首元素 / 删除。
- 检测特征：日志出现 `type must be string, but is array` + 紧接
  "Printer model is missing" 即本判例（勿按打印机型号排障）。

## 修复方案（待批，未实施）

`load_from_json` 特殊键分支加 `is_array()` 容忍（取首元素，与 BBL 键
语义一致——同值数组本就是每槽重复）。属 fork 级上游硬化候选，与 87fa
`normalize_filaments` coEnums 构造器缺陷（P-01 附案）同族：**新版写盘
格式演进 × 旧读侧假设**。

## 伴生观察（2026-10-29 已定案：陈旧二进制假象，P-28 无回归）

首次诊断时（10-28，build-static 10-26 构建）本判例失败路径（rc=6 +
显式 `-j`）**未落盘 JSON**，与 P-28 契约矛盾，且 stdout 无
`=== SLICE STATISTICS ===`——走的竟是 P-28 前旧守卫。深挖发现该二进制
**选择性陈旧混链**：主二进制含 `error_code` 改名（7c82968）却缺
8f729b5 的无条件写行为；测试二进制缺 10-19 已入库的 P-28 用例
（`--list-tests` 实数核对 0 命中）。`cmake --build -j 2` 重建后（自报
v2.2.1 与 HEAD 一致）复测：本 fixture rc=6 且 JSON 落盘
（`error_code="PRINTER_MODEL_MISSING"`、`output_file=""`，P-28 语义
完好）；P-28 标准场景（空资源目录）同样落盘。**结论：P-28 契约无
回归，前述矛盾是陈旧构建假象。**

教训（已入 P-14）：结论入判例前，凡涉"某修复行为缺失"，先重建并
`--list-tests` 核对新用例在场，再定性"回归"。（本机陈旧对象混链第
三次，见 P-14 案例 list。）
