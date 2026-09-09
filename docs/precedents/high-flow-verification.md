# P-13: 高流量生效三层判定（验证方法论）

**状态：** 2026-09-04 实测定稿；测试锚点：test_high_flow.cpp

从 gcode 判断高流量是否**真正生效**（而非仅声明），三层证据递进：

1. **声明层（必要不充分）**：`; filament_volume_type = high_flow,...` 等。
   上游"flow 数据缺失/不匹配时**静默回退 standard 不报错**"——引擎返回
   成功 + 声明键照旧存在。单看此层会被骗。
2. **重映射层（最直接）**：耗材域参数已按槽分段取 high_flow 项（如
   `filament_max_volumetric_speed` 槽 5 = 40 而 standard 档 18）。
3. **实测层（不可伪造）**：统计 `T n` 之后 `G1 X..Y..E` 挤出移动的 F 值，
   最大挤出速度 **> standard 档上限**（如 >200/270 mm/s）→ 只能来自
   high_flow 档。注意常被 fmvs 体积上限压制（20 mm³/s ≈ 274 mm/s 封顶），
   略超 standard 上限即是证据。报告结论时引用第 3 层数值。

实现：commit fdcff12 端到端支持 high-flow flow-variant presets。
相关：P-06（透传即终态）。
