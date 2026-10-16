# P-11: auto_brim 云引擎/桌面版行为差异（根因分析完成，未修复）

**状态：** 未修复（根因分析 2026 完成，修复方案待定）

## 现象

用户报的"裙边"实际是 **BRIM**（skirt_loops=0 已正确生效）。`brim_type=
auto_brim` 时桌面版**无**底边，云引擎**生成**底边（gcode 有 ~420 行
`;TYPE:Brim` 挤出）。

## 决策链（Brim.cpp:761-806 configBrimWidthByVolumeGroups）

```
brim_width = adhesion * min(min(max(
    height_to_area * maxSpeed,
    thermalLength * 8/thermalLengthRef * min(height,30)/30), 18.), 1.5*thermalLength)
if (brim_width < 5 && brim_width < 1.5*thermalLength) brim_width = 0   // 小底边丢弃
```

桌面版算出 <5 → 丢弃；云引擎算出 ≥5 → 且 `:965` 处 `auto_brim=0` 才回退
用户 brim_width=5。差异候选来源：首层切片几何（受 ensure_on_bed Z-bake
修改 mesh 顶点影响）、adhesion 系数、bounding box。

## 修复方向（三选一，未定）

1. 深入对比 configBrimWidthByVolumeGroups 输入参数（治本）
2. 引擎侧强制/覆盖 brim_type（简单，但改变行为）
3. Process 预设覆盖时把 brim_type 纳入覆盖
