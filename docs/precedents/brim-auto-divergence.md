# P-11: auto_brim 云引擎/桌面版行为差异（已修复：运动对齐 + 回显保真）

**状态：** 已修复（2026-09-18，两阶段修复：48d6e15 运动对齐 → 本次回显保真）

## 现象（第一轮，2026-06）

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

桌面版算出 <5 → 丢弃；fork 的 libslic3r 在算法返回 0 时**回退用户
brim_width**（Brim.cpp:968-971），云引擎因此多画。**根因是 fork 与桌面
2.4.0 的 fallback 语义不同**，不是几何/adhesion 差异。

## 第一轮修复（48d6e15，2026-06-30）

`apply_auto_brim_fallback()`：auto 模式下把 m_config.brim_width 清 0 →
fork fallback 拿到 0 → 不画。运动对齐桌面达成（5 用例桌面对照实测：
基准 ABS/PLA brim bbox 逐坐标一致、立方体/0.6mm 两端都无裙边）。

## 第二轮问题（2026-09-18 云切片审查报告）

清零作用于 m_config 单一配置对象，而 m_config 同时喂三个出口：
切片（正确要 0）、gcode 头部回显（应保留 5）、输出 3mf 的
project_settings.config（应保留 5）→ 后两者被污染成 0（37/37）。
桌面 2.4.0 是"回显如实=5 + 运动纯算法"——引擎变成了"回显说谎=0"。

**为何不能简单删清零**：删掉后 fork 的 fallback 语义生效 → 立方体/
0.6/0.8 用例会画出 5mm 裙边而桌面不画（skip 实验实证 5.13mm）——运动
回归。且 config brim_width 在 fork 里还有多个消费点（Brim.cpp:207-212
面积通道、Brim.cpp:61、PrintObject.cpp:698；~~Print.cpp:2168 margin 门~~
经 2026-09-20 复核在 `#if 0` 死代码块内，不参与编译），fork 侧语义复制
风险高。

## 第二轮修复（2026-09-18，本次）

把"清零"从 m_config 挪到**切片副本**，回显出口恢复项目值：

1. `prepare_merged_config_for_plate()`：merged_config 上 auto 清零
   （pd->config 覆盖之前，保持旧时序语义）→ print 输入与修复前逐位相同
2. `restore_brim_width_echo()`：export_gcode 后把 gcode 文件中
   `; brim_width = ` 行改写回 m_config 值（仅 auto 触发，流式重写）
3. 输出 3mf：`params.config = &m_config`（未清零）→ 自动恢复 5

**验收标准**：全部用例运动 gcode 与修复前逐行一致，唯一差异
`; brim_width = 0` → `= 5`（含 3mf 内嵌 config）。

**测试**：test_preset_substitution 不变量更新（旧断言是死代码——
`option<ConfigOptionString>` 对 enum/float 类型错配恒 nullptr，从未执行
过）；test_high_flow 新增 gcode 级回归（echo=5 且 TYPE:Brim 保持
absent，动到任一侧都会红）。

## Stage 2.1 加固（2026-09-20 对抗审查跟进）

> 完整记录（bug/根因/方案/测试/遗留）见 **P-20：
> brim-echo-restore-hardening.md**；本节为摘要。

三视角×两轮循环对抗审查（harness 协议）确认第二轮修复行为正确
（print 输入逐位相同、无第三回写面），但 `restore_brim_width_echo`
实现带 1×P1 + 3×P2 缺陷，本次加固：

1. **提交门（P1）**：close 后校验 `in.bad()/out.good()`，I/O 失败
   （ENOSPC/EIO）时保留原文件——原先截断的 tmp 会静默 rename 覆盖
   完好 gcode 并打包交付（上游 GCode.cpp:1812-1816 的 flush+is_error
   是仓内范本）
2. **异常自含（P2）**：整体 try/catch 降级 warning——注释行改写失败
   不再经 all-or-nothing 打包放大为整单 GCODE_EXPORT_ERROR 报废
3. **tmp RAII（P2）**：`TmpFileGuard` 覆盖全部退出路径——原先异常/
   被杀路径泄漏全尺寸 `.brimecho.tmp`（single_plate GCODE 模式漏进
   输出目录）
4. **锚定（P2）**：只在 `; CONFIG_BLOCK_START/END` 区间内且旧行值为
   "0"（清零副本的确定产物）时替换——不受信 3MF 透传的 machine
   gcode 无法再劫持首行匹配（非 BBL 打印机真回显块在文件尾，
   GCode.cpp:3100）；`brim_width==0` 时整体早退免全文件空转

测试补强：brim 回归用例新增输出 3mf `project_settings.config` 断言
（`"brim_width": "5"`，JSON 格式——原验收点"含 3mf 内嵌 config"此前
无自动化覆盖）；preset invariant 软守卫 `if(bt)` 改 REQUIRE（防再次
静默死掉，旧断言以此机制空转 6 周）。

**审查同时证伪的假设**（留档防复发）：ostringstream 与回显序列化分歧
（`ConfigOptionFloat::serialize` 同为 ostringstream<<，Config.hpp:578）；
pd->config 盘级 brim 键错配（bbs_3mf 导入器固定键集不可达）；无尾换行
+1 字节（fork GCode.cpp:3115 无条件尾 `\n`）。
