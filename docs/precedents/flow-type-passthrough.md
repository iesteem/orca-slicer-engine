# P-06: 流量类型（flow type）透传即终态（宪法级决策）

**状态：** 用户确认（2026-09-03），**不要再建议加选择 API**

云引擎**不需要**高流量耗材的选择能力。流量类型选择是桌面 GUI 行为
（FlowTypeHelper → `set_filament_volume_types`），用户在桌面导出 3MF 前已
选好，`filament_volume_type` 随 3MF 携带。引擎侧 `get_config_idx()` 在
G-code 阶段自动按该值取向量化速度键元素，透传即正确。

## 判定规则（四组 fixture 实证）

**切片变体 = 打印槽 filament_volume_type 在 flow_support 中的索引；
nozzle_volume_type（喷嘴硬件）完全不参与切片路径**（仅 GUI 校准向导用）。

## 高流量是否真生效的三层验证（P-13）

详见 [high-flow-verification.md](high-flow-verification.md)。回归保障靠
tests/test_high_flow.cpp 的进给速率断言（>500 mm/s）——因为解析失败会
**静默回退 standard 且仍返回成功**。
