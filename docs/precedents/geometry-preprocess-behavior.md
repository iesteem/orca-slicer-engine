# P-12: 几何预处理 / ensure_on_bed / build-volume 行为基线

**状态：** 行为基线（2026-08-07 实测）；测试锚点：test_geometry_preprocess.cpp、
test_build_volume_classify.cpp

## Z-bake 核心契约

预处理后**所有 case 的 instance Z offset 恒为 0**。Z≥0 → bake 后落床
（min_z≈0）；Z<0 或部分沉床 → 保留（allow_negative_z=true 语义）。

| fixture | 对象数 | min_z / max_z | 行为 |
|---|---|---|---|
| geom_above_bed_ok.3mf | 1 | 0 / 27 | 落床 |
| geom_below_bed_mostly.3mf | 2 | -26.95 / 27 | 下沉保留 |
| geom_partial_sink_ok.3mf | 1 | -3 / 24 | 部分沉床保留 |
| geom_fully_below_bed.3mf | — | — | 抬升 + OBJECT_BELOW_BED_ADJUSTED warning |
| geom_too_high.3mf | 2 | 0 / 540 | 超高（build-volume 阶段处理，预处理阶段不失败） |

**超高/越界不在几何预处理阶段失败**——那是 build-volume 检查
（process_plate 内）的事。

## 几何缺陷检查

7 项几何检查（SELF_INTERSECT/MULTI_COMPONENT/ZERO_VOLUME 等）**全为
warning，不阻断**；`has_geom_error` 阻断分支是**死代码**。全模型实测锚点
（geom_defects_cabin，573 万三角形）：SELF_INTERSECT=51、
MULTI_COMPONENT=35、ZERO_VOLUME=8，全部 warning，any_error=false。
断言符号不变量，不绑具体数字。

## u1_preset_test.3mf 特征

单对象单实例单盘，instance Z=13.5 → 命中 ensure_on_bed Z-bake 分支；
多实例/多盘分支无法用此 fixture 触发。
