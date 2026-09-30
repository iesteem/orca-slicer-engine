# 测试分层：行为锚点 vs 实现细节

> 用途：下次整体重构（"转生"）时，**[anchor] 层测试必须原样通过**（允许
> 迁移到新测试框架，断言语义不可变）；[impl] 层测试随旧实现一起废弃，
> 只作参考。分层依据是断言对象：断言"输入→可观察输出契约"的是锚点，
> 断言"内部结构/函数返回"的是实现细节。

| 测试文件 | 层 | 依据 |
|---|---|---|
| test_slice_e2e.cpp | **[anchor]** | 断言 exit code + JSON 产物契约（run() 全流程） |
| test_file_validation.cpp | **[anchor]** | 输入文件格式 → exit 3 + issue code 契约 |
| test_printer_reject.cpp | **[anchor]** | 非 U1 打印机 → 拒绝契约 |
| test_gcode_security.cpp | **[anchor]** | `post_process` 恒空（RCE 硬契约）+ 官方 G-code 覆盖 |
| test_filament_count.cpp | **[anchor]** | per-plate extruder 集合 + 克重 |
| test_accuracy.cpp | **[anchor]** | 成功基线 + 多色结构（回归基线） |
| test_boundary.cpp | **[anchor]** | 喷嘴/层高/温度越界契约 |
| test_layer_height.cpp | **[anchor]** | 层高优先级（用户 vs 预设） |
| test_temp_mode.cpp | **[anchor]** | 高低温混用失败契约 |
| test_path_conflict.cpp | **[anchor]** | TOOLPATH_CONFLICT / SPIRAL_LIFT |
| test_unsupported.cpp | **[anchor]** | 逐件打印/可变层高拒绝契约 |
| test_high_flow.cpp | **[anchor]** | P-13 实测层进给速率断言（>500 mm/s） |
| test_print_sequence.cpp | **[anchor]** | print_sequence 混合盘成败契约 |
| test_slicing_deadline.cpp | **[anchor]** | 超时行为契约 |
| test_geometry_preprocess.cpp | mixed | Z-bake 不变量是锚点；`model()` 访问器是 [impl] |
| test_build_volume_classify.cpp | mixed | 分类结果是锚点；分类函数签名是 [impl] |
| test_validate_classify.cpp | mixed | 同上 |
| test_preset_substitution.cpp | mixed | 替换结果契约是锚点；PresetManager 内部路径是 [impl] |
| test_filament_rollback.cpp | mixed | 回退结果契约是锚点；PresetRollback 内部查找是 [impl] |
| test_types.cpp / test_utils.cpp | **[impl]** | 工具函数单元测试，随实现废弃 |
| test_path_conflict 内部辅助 | **[impl]** | 同上 |

## 重构时的验收协议

1. 新实现必须在全部 [anchor] 用例上通过（fixture 不变）。
2. [anchor] 断言与桌面版 OrcaSlicer 实测行为冲突时，以判例库
   （docs/precedents/）为准，判例可推翻测试，测试不可悄悄偏离判例。
3. [impl] 用例失效不阻塞重构，但其覆盖的内部逻辑变更须在 CHANGELOG 记录。
