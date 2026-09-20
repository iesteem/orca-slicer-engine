# 行为判例库（Precedents）

> 本目录是本项目的"不死判官"：记录每一条**已实测确认的行为契约**及其根因。
> 下一代实现（无论人写还是 AI 重写）必须遵守这些判例；判例只能被人类
> 明确批准后才能推翻。每条判例尽量带 fixture 引用与验证证据。

## 格式

每条判例一个文件（或合订），包含：现象 / 根因 / 修复 / 验证证据 / 已证伪的
推测（防止重新走弯路）。

## 索引

| 编号 | 主题 | 文件 | 状态 |
|---|---|---|---|
| P-01 | 3MF 缺配置键 → null deref 崩溃 | crash-missing-config-keys.md | 已修复（defaults backfill） |
| P-02 | flush_volumes_matrix 维度不匹配 → ToolOrdering SIGSEGV | crash-flush-matrix-mismatch.md | 已修复（fe5c61c） |
| P-03 | 高度范围换色擦塔缺失 | wipe-tower-layer-config-ranges.md | 已修复（上游 5cac51bbe5） |
| P-04 | modifier 多色被误 trim 单色 | modifier-extruder-trim.md | 已修复（trim 整体移除） |
| P-05 | all-or-nothing 打包 | packaging-all-or-nothing.md | 宪法级决策，非 bug |
| P-06 | 流量类型透传即终态 | flow-type-passthrough.md | 宪法级决策 |
| P-07 | toolpath_outside 无头移植三坑 | toolpath-outside-headless.md | 已修复（2026-08-17/18） |
| P-08 | 擦塔位置 clamp 缺失 | wipe-tower-position-clamp.md | **未修复**（含修复方案） |
| P-09 | --skip-preset-substitution 崩溃 | skip-substitution-nullderef.md | **未修复** |
| P-10 | 5479 ToolOrdering 堆损坏 | toolordering-heap-corruption.md | 搁置（上游 bug，含已证伪修复） |
| P-11 | auto_brim 云/桌面行为差异 | brim-auto-divergence.md | 已修复（2026-09-18 两阶段：运动对齐 48d6e15 + 回显保真） |
| P-12 | build-volume / 几何预检行为表 | geometry-preprocess-behavior.md | 行为基线 |
| P-13 | 高流量生效三层判定 | high-flow-verification.md | 验证方法论 |
| P-14 | 环境与构建约束 | build-environment-constraints.md | 强制约束 |
| P-15 | 桌面版 vs 云引擎配置流程 | cloud-vs-desktop-config.md | 参考架构 |
| P-16 | inherits_group 缺格式断言 | inherits-group-format-guard.md | 降级 warning（parity 决策，勿重开） |
| P-17 | nozzle_volume_type 被官方打印机预设整体覆盖重置 | nvt-reset-and-output-relocation.md | 已修复（3640a49，2026-09-20） |
| P-18 | 输出路径被占用时静默换名（时间戳后缀） | nvt-reset-and-output-relocation.md | 已补可观测性（af80f0b）；换名行为按设计保留 |
| P-19 | 空盘 rc=0 假成功（零产物退出码） | empty-plate-zero-product-rc.md | 已修复（2026-09-20，选项 A 良性跳过语义） |
| P-20 | restore_brim_width_echo I/O 失败语义与劫持面 | brim-echo-restore-hardening.md | 已修复（2026-09-20 stage 2.1，f291619 对抗审查跟进；含遗留 P3 清单） |
| P-21 | 预设替换向量短侧截断（含上游 set_num_filaments no-op 发现） | vector-short-side-truncation.md | 已修复（2026-09-20，C3；上游 init_filament_option_keys 零调用待上报） |
| — | 测试分层：锚点 vs 实现细节 | test-layering.md | 重构验收协议 |
