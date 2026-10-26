# P-31: wholesale 替换跳过缺失键，官方值被 FullPrintConfig 默认值冒充（已修复）

**状态：** 2026-10-21 修复（W2 批次复跑分诊驱动）；判例族 P-15/P-30 的第三例，收束"替换语义"家族

## 现象（W2 批次实测）

3664 例（擦除矩阵，3MF 未携带 machine 级键）引擎输出与桌面基线 diff 中出现一组
"engine=代码默认值 / desktop=官方 U1 预设真值"的键：

| 键 | 引擎（错） | 桌面（官方真值） |
|---|---|---|
| `enable_filament_ramming` | 1（PrintConfig 默认 true） | 0（U1 预设声明） |
| `purge_in_prime_tower` | 1 | 0 |
| `extruder_clearance_radius` | 40 | 72.5 |
| `machine_tool_change_time` | 0 | 5 |
| `tool_change_temprature_wait` | 1 | 0 |
| `ramming_pressure_advance_value` | 0 | 0.02 |
| `bbl_use_printhost` | 缺 | 0 |

## 根因链

1. `m_config` 在替换时**只含 3MF 写过的键**（P-29 已记录：Model::read_from_file 直填，
   defaults backfill 在 `normalize_loaded_config` 才跑，位于替换之后）
2. `overwrite_all_keys_from_impl` 旧语义"只覆盖 dst 已有键"（`option(key, false)`）：
   official 预设声明、3MF 未携带的键**被跳过，官方值进不去**
3. 随后 backfill 建键时填 **FullPrintConfig 代码默认值** → 默认值冒充官方预设值
4. 同体系不一致实锤：filament 路径 `PresetRollback::overwriteExtruderFrom` 用
   `option(key, true)` 一直创建缺失键；printer/process 路径不创建

时序：`apply_preset_substitution()`(L297) → `normalize_loaded_config()`(L305)
——替换先、backfill 后，正好让漏网键落进默认值。

## 影响

- **塔族放大器**：ramming=true 使塔深 +2mm（P-27 实测同值），是 nightpetal/5215
  rc=7 TOOLPATH_CONFLICT 的贡献因素之一（非唯一：几何交叠本身仍在，见 P-08 族）
- **附带修复（意外收益）**：printer 覆盖创建键后紧跟的
  `normalize_filament_vectors_to_slot_count`（L1344）把新建 vector 键 resize 到
  **项目槽位数**，W2 发现二的"槽数不稳定"形态（modifier fan 8v4）同步收敛
- 3664 diff 59→52 键（恰减本表 7 键，无意外新增）

## 修复

`overwrite_all_keys_from_impl`（SliceEngine.cpp）`option(key, false)` →
`option(key, true)`：src 有而 dst 缺的键**创建**（`DynamicConfig::optptr` 经
PrintConfig def 创建正确类型默认实例后 `set(src)`；def 未知键返回 nullptr 由
原 guard 跳过）。printer（L1336）与 process（L1784, except 集合语义不变）两
路径统一，对齐 filament 路径与桌面 `ConfigBase::apply` 语义。

## 验证锚（2026-10-21 实测）

- 3664：上表 7 键全部对齐 desktop；diff 59→52
- modifier：fan_min_speed 8→4（=项目槽）；数组差 54→53
- nightpetal：fan_min_speed 4（=项目槽；desktop 基线 8=机器 AMS 槽，锚不稳定，
  见下方"序列化槽数契约"）
- 回归锚全绿：layer_height 例 bridge_flow=1/layer_height=0.19；rot fixture
  rotation=270（P-30）；engine-tests 74 用例 211 断言全绿

## 序列化槽数契约（W2 发现二的答复口径）

引擎 project_settings 数组键长度规则（修复后稳定）：
1. **per-filament 键 = 项目槽位数**（`filament_settings_id.length`）
2. **flow-variant 键**（`process_flow_variant_options()`：速度/加速度/jerk 族 +
   `filament_flow_step_size`/`filament_flow_support`/`filament_volume_type`）=
   **flow 段数**（standard/high_flow 分段，官方预设自身声明 `["200","500"]`
   形态，读时经 ConfigFlowDomain 解析）——不是槽位数，下游解析必须按段语义
3. 桌面侧槽数不稳定（modifier 输出 4=项目槽，nightpetal 输出 8=机器 AMS 槽，
   同为 4 槽源 3MF）——**不建议继续以桌面数组长度为 parity 锚**；以本契约为准

## 定量锚：3664 克重膨胀 +19.1/+16.6/+23.3% 的完整分解（2026-10-23，回执2 复现）

QA 回执2 报告 3664 盘1 三槽克重偏差 +19.1/+16.6/+23.3%，超出其密度算术
（±6%），疑"擦料塔耗材分配在替换后重算"请求单独定性。实测证伪塔重算假说，
偏差为**两因子乘积**，旧构建因子即本判例冒充键：

- `b2e0e60` 重建（= QA 服务器 2026-10-21 10:09 二进制源时点，早于当日
  17:55 的本修复）：**逐位复现** 9.74/14.51/17.31g（对桌面 8.18/12.44/14.04）
- 分解：×1.0645（密度回退 1.24→1.32，均匀）× 塔耗材 **+11.8~13.1%**（本
  判例 `enable_filament_ramming`/`purge_in_prime_tower` 冒充为 1 → 331+
  换料每次向塔多计 ramming/purge 体积）
- 分区实测（gcode `Prime tower` 段逐工具）：旧构建**对象侧挤出逐位不变**，
  多出的 326/400/747mm **全部在塔内**
- HEAD（≥3eced92）：挤出长度与桌面逐位一致（≤0.007%）、塔逐工具 +0.00mm、
  克重均匀 +6.4% = 1.32/1.24；`flush_volumes_matrix` 源=引擎输出=桌面 4×4
  逐值相同（矩阵不被替换）
- 密度方向注记：回退目标是**官方同名族预设**（Snapmaker PLA Basic @U1，
  ρ1.32），非 Generic PLA（1.24）——"回退应偏轻"直觉在此族不成立；
  QA 9143/6107 的 +6.5% 同理（= 1.32/1.24−1），回执3 已闭环（下节）

## 9143/6107 结案（2026-10-23，QA 回执3 长度对照自检）

QA 以"引擎 json used_mm vs 桌面 slice_info used_m→mm"长度对照代替源件
逐槽确认（输出侧证据，更强）：9143 槽1/3/4(盘2) = +1.9/+0.3/−0.0%，
6107 槽1 = −0.03%，全部 ≤2%。引擎侧复算：9143 三槽聚合长度
40738.93 vs 40720 = **+0.046%** ⇒ 克重 +6.5% 几乎全由密度因子
（1.0645）解释，与本案 HEAD 侧 3664 均匀 +6.4% 定论互证。两例即刻
结案；3664 待 QA 服务器构建升级后复测（预期收敛为纯密度 +6.4%）。
注记：桌面基线 used_m 为 ~3 位有效数字舍入值（600/3370/36750），
单槽长度差含舍入噪声（槽1 +1.9% 在该量级），聚合侧不受影响。

## 回执 4（2026-10-26）：QA 新构建复测"收敛一半"的解剖

QA 10-24 07:32 构建复测 3664：三槽长度仍 +5.95/+4.80/+7.93%、克重
+12.7/+11.6/+14.9%，判"未收敛"并疑"修复不完整或构建过旧"。解剖结论：

**① 构建时点问题排除（P-31 确定在构建内）。** P-31（3eced92）与 P-32
（9cc51b7）同批提交（10-21 17:55）、同一次推送（10-21 18:20，随 88a87da）；
回执 4 已确认该构建 P-32 生效（槽3 收敛）、P-33 生效（其修复在更晚的
64d7d0e，10-22 16:23 推送）——含 P-32/P-33 的构建必含 P-31。该构建 =
本仓 10-23 17:57 的 build-static（= b3b761d 时点，**早于同日 15:11 的
P-35 S2**）。

**② 残差 = 旧构建塔内多余量的精确 ½（离散机制指纹）。** QA 长度目标 =
桌面基线（2741.75/4171.19/4706.39，与重量目标 8.18/12.44/14.04 同源，
两组偏差算术互证）。新构建塔内多余 = 163.1/200.2/373.4mm，与旧构建多余
326/400/747mm 之比 = **0.500±0.002（三槽全部）**——不是"部分收敛"，是
某项按换料次数计的塔体积贡献**精确减半**。克重偏差 = (1+长度偏差)×
1.32/1.24 逐位成立：密度项（回执 3 已结案）完整解释克重-长度差。

**③ 机制层修正：两个机器键在 U1 上是死键，本案旧构建塔膨胀的归因需修正。**
`enable_filament_ramming` 仅在 SEMM 生效（WipeTower2.cpp:2044
`m_semm && ...`），`purge_in_prime_tower` 同样 SEMM 门控（:3057）；U1
`single_extruder_multi_material=0`（fdm_U1.json）→ 两键双双无效。上表
"冒充为 1 → 塔多计 ramming/purge 体积"的因果链不成立（键确实冒充了，
但冒充无后果）。U1 上真正驱动塔 ramming 的活键是耗材级
`filament_multitool_ramming`（PrintConfig 默认 false；Basic @U1 叶子声明
1/volume 5/flow 25，base 声明 0/10/10，Generic PLA 声明 1/0.5/20）。

**④ 螃蟹实证锚（机械螃蟹.3mf，本地 HEAD，替代 3664——源件已随回收站清理
丢失，待 QA 重发）：**
- ramming **开关**有效：Generic PLA 声明 1→0，plate2 长度
  −572.6/−1001.5/−1077.1/−565.0mm（−1.2%~−5.0%，逐槽不均 = 塔内集中，
  与 QA 残差同构）
- ramming **volume/flow 无效**：回退到 Generic PLA（v0.5）vs Basic @U1
  （v5）输出逐位几乎相同（±1mm）——`overwriteExtruderFrom` 自 e2e2ac9
  （P-21，2026-09-20）起跳过流变键（19 键族，ramming volume/flow 经
  `get_value_at(ConfigFlowDomain::Filament)` 读取），预设值进不去，
  运行时用 3MF 携带值/默认值（实测 effective volume=10=代码默认）
- P-35 S2（4e6b18e，10-24 15:11）补桌面 load_external_preset 对应的流变
  键恢复，但仅对 nil/stale 携带数据触发——螃蟹携带有效数据，post-S2 重建
  （479b62c，与 package/orca-slice-engine MD5 一致）输出无变化；
  **QA 构建早于 S2 8 小时**，若 3664（"(converted]" 工艺 = 转换器导出高危
  形态）携带 nil/stale 流变数据，S2 在其路径上有真实作用，下个构建复测
  有变化空间
- JSON used_mm ≡ gcode 尾部 `filament used [mm]` 逐位一致（尾部公式 =
  实测 + `wipe_tower_data.used_filament[]` 塔计划估算，SliceEngine.cpp:3584
  取 `total_volumes_per_extruder`）——统计通道无独立偏差

**⑤ 复现缺口 = 输入发散，非代码分歧。** 同一代码（10-23 验证 @816e4ee ≡
QA 构建 ≡ b3b761d，期间仅 docs + P-35-1 螺旋修复不涉塔）+ 同版本串资源
（02.03.04.04）下，本方 10-23 实测 3664 逐位一致、QA 新构建 +4.8~7.9%——
代码相同而输出不同 ⇒ 输入不同：QA 用例库的 3664 源件副本或其 `-r` 资源
副本与回执 2 证据包/仓库资源有差异（版本串只读 Snapmaker.json，预设文件
内容可异）。待 QA 提供：源件 sha256/文件、资源目录（至少 Basic @U1
leaf+base 与 U1 机器预设）、或在其服务器重跑我方验证过的证据包原件。

## 关联

- P-15（云 vs 桌面配置地图）、P-29（backfill 时序佐证）、P-30（process 路径
  4 键豁免；本案是同族"官方值进不去"的通用形态）、P-08/P-26/P-27（塔族，
  ramming 冒充是放大器）、P-35（S2 流变键分段改写；本案④为其作用面之一）
- 回退路径（FILAMENT_ROLLED_BACK）的 wholesale 替换**用户耗材参数值**属
  P-15 有意设计（云安全），不在本案范围，待产品豁免清单决策
