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
| P-08 | 擦塔位置 clamp 缺失 → 误报 TOOLPATH_OUTSIDE | wipe-tower-position-clamp.md（塔族合订） | **未修复**（含修复方案 + 用户侧绕行已验证） |
| P-09 | --skip-preset-substitution 崩溃 | skip-substitution-nullderef.md | 已修复（参数已移除，2026-09-29） |
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
| P-22 | 无 plate 元数据 3MF 假成功静默失败 | plateless-3mf-synthesis.md | 已修复（21b4ef1，含黄金基准与已证伪方案；2.1.7 侧原编 P-16，合并入 2.2.0 线时让号） |
| P-23 | 盘元数据无成员关系 → 假空盘零产物（MakerWorld/Orca 2.4.2 新格式） | makerworld-membership-rebuild.md | 已修复（2026-10-08，几何重建桌面语义 + NO_PLATES_PRODUCED 可观测性） |
| P-24 | 非空 membership 与几何错位 → 3 盘误判越界零产物（always-rebuild，人类批准推翻 P-23 收敛） | always-rebuild-plate-membership.md | 已修复（2026-10-10，always-rebuild + INSTANCE_NOT_ON_ANY_PLATE warning + tip 静默路径） |
| P-25 | 输出 3MF 盘元数据 `support_used` 换源 gcode 实际层（人类批准推翻 D1 parity；全 fork 树零真实消费方） | support-used-3mf-metadata-source.md | 已落地（2026-10-10，契约决策；JSON `support_free` 与 3MF `support_used` 互补同源） |
| P-26 | 3MF 存储塔位与对象 bbox 重叠 → TOOLPATH_CONFLICT z=0（引擎检测正确，缺桌面自动摆塔） | wipe-tower-position-clamp.md（塔族合订） | **未修复**（2026-10-19；移塔/移模两路绕行均实测 rc=0） |
| P-27 | V-FM01b 双 bug 定稿：内嵌脏预设 `enable_filament_ramming` 透传 → 塔深三盘恒 +2.00mm（Bug A，单键注入钉死）；检测器对 0.137mm brim 亲吻报 CONFLICT 而桌面同几何不报（Bug B，rc=7 触发器） | wipe-tower-position-clamp.md（塔族合订） | **A 待修**（规范化方案已定）；**B 待桌面 2.4.0 源码对照**；purge/塔位/矩阵/前缘四假说均已证伪在案；另 #FFFFFF 槽不现独立缺口未查 |
| P-28 | 显式 json_output_path 无条件写文件：早期失败（rc=3/6）output_path 恒空连显式路径也被守卫跳过；2026-10-13 防垃圾文件决策收窄至"未传路径的直调方"；伴生裸 `-j` 空串不落盘对齐 | explicit-json-path-unconditional-write.md | 已修复（2026-10-19，宽语义用户批准；CLI 失败恒落盘，rc=1/99 仍无文件） |
| P-29 | 加固函数自身死于缺键输入：sanitize_layer_config_ranges 对缺 layer_height 的裸 opt_float 解引用 | sanitizer-missing-key-self-crash.md | 已修复（含 e92c7da 二次 null 兜底，2026-10-22） |
| P-30 | 工艺预设 wholesale 替换把 3MF 塔旋转角 270° 冲成默认 0°（B1+g 门控豁免 4 键） | wipe-tower-rotation-preset-overwrite.md | 已修复（2026-10-21；同族 support/wipe_tower 耗材键豁免待产品） |
| P-31 | wholesale 替换跳过缺失键，官方值被 FullPrintConfig 默认值冒充 | preset-keys-backfill-masquerade.md | 已修复（3eced92，2026-10-21） |
| P-32 | slice_info 导入残留叠加：旧引擎产物污染源 3MF + parse 不清空 | slice-info-import-residue.md | 已修复（9cc51b7，2026-10-21） |
| P-33 | 温度混用检测不收集 layer_config_ranges——高度范围换高低温槽被放行（引擎有意比桌面更严，P-30 路径 A 的姊妹案） | layer-range-temp-mixing-gap.md | 已修复（2026-10-22，W2 二批问题三路径 B；上游 issue#5 族待对齐） |
| P-34 | 病态 infill 内存爆炸：sparse infill 退化为微米级段，G-code 生成段 ~19MB/层无界增长（Faceted_Bowl_2，桌面同件 exit 2） | degenerate-infill-memory-blowup.md | 未修复（部署层缓解：cgroup/看门狗，引擎无护栏） |
| P-35 | `_do_export` 用覆盖前 full_print_config 快照冲掉 print.config() 的耗材域 retract 覆盖（fork 376524d9a6 顺序反了）→ U1 机器 Auto 基值换层强制螺旋整圆扫出热床固件拒打；波及整个 extruder_retract 覆盖键族；S2 跟进：官方 flow-variant 段重写关闭 nil/旧值 3MF 类缺口 | gcode-export-override-clobber.md | 已修复（2026-10-23，Up 侧重排序 + 引擎告警有效值化 + S2 `overwrite_flow_variant_segment`；回显=覆盖前快照的陷阱在案） |
| — | 测试分层：锚点 vs 实现细节 | test-layering.md | 重构验收协议 |
