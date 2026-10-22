# P-08 / P-26 / P-27: 擦塔判例族（位置 clamp 缺失 / 塔位重叠 CONFLICT / ramming 透传 + 检测器分叉）

> 本文件为**塔族合订判例**（文件名沿用 P-08 单案时期，保持链接稳定）。
> 共同根因面：桌面 GUI 在渲染/保存时重算塔位并**回写** project config，
> 引擎无 GUI，直接消费 3MF 存储值（libslic3r `Print.cpp:1498` 的
> `wipe_tower_x.get_at(plate_index)`；src/ 引擎代码零处引用
> wipe_tower_x/y，无任何重定位）。
>
> | 编号 | 症状 | 状态 |
> |---|---|---|
> | P-08 | 塔位超床 → TOOLPATH_OUTSIDE（0.21mm 越界） | 未修复（WIP 在 stash） |
> | P-26 | 3MF 存储塔位与对象 bbox 重叠 → TOOLPATH_CONFLICT z=0（引擎检测正确） | 未修复（用户侧绕行已验证） |
> | P-27 | V-FM01b 双 bug：enable_filament_ramming 透传致塔深 +2mm（A，根因钉死）+ 检测器 0.137mm brim 亲吻分叉（B，桌面侧未 diff） | A 待修（规范化方案已定）；B 待桌面源码对照 |

---

## P-08: 擦塔位置 clamp 缺失 → 误报 TOOLPATH_OUTSIDE（未修复）

**状态：** 根因已定位（2026-09-09），**未修复**（v02.01.06 决定不处理；
WIP 存于 git stash `stash@{0}`，含直移植 clamp + WTCLAMP 探针）
**Fixture：** `tests/fixtures/2c242c10f0e4b521.3mf`（已入库，md5
`3dba3d258052e3d4503bd1514d15ce77`）；库内另有
`wipe_tower_toolpath_outside.3mf`

### 2026-09-09 移植尝试的关键发现（WIP 未完成的原因）

直接移植桌面 clamp（`y > bed_max − depth − margin` 则回推）**不够**：

- 引擎实测切片后真实 depth = **19.5**，但塔 gcode 实际 Y 向延展 ≈
  **26.5mm**（anchor 244.697 → 顶 271.21）——超出部分来自 brim(5) +
  塔底锥度扩展（cone base）。
- 引擎 printable_area bbox max.y = **271**，桌面渲染 clamp 用的 plate
  bbox（bed shape）max.y = **270**、depth 用估算值 **21.5**（估算路径
  prime_volume 30×3/(0.2×35)≈12.86 → min_depth_per_height 下限 20）——
  两套数字各自成立但都推到 242.5。引擎侧用"实测 depth 19.5 + 271"算
  出 270.2 ≤ 271 不触发 clamp，而真实塔顶 271.21 越界。
- **下一步**：clamp 的塔延展量不能只用 depth，需含 brim + cone base
  （参考 `Print::first_layer_wipe_tower_corners`，Print.cpp:2985-3005，
  `WipeTower2::get_wipe_tower_cone_base`），或直接用该 corners 多边形
  做 bbox 级 clamp。

### 根因

桌面 GUI 渲染塔预览时（GLCanvas3D.cpp:3856-3914）把塔位置 clamp 进盘内并
**回写** wipe_tower_x/y 到 project config。引擎无 GUI，盲信 3MF 的
wipe_tower_y（实测 244.697 vs 桌面 clamp 后 242.5），塔顶超界 0.21mm。

桌面 depth 计算：`estimate_wipe_tower_size`（PartPlate.cpp:1687）=
max(flush_depth, min_depth_per_height 插值)（{100→20, 250→40}，
WipeTower.cpp:602）；clamp 公式 `y = plate_max − depth − margin(1+brim)`。

### 修复方案（已定，未实施）

在 `process_plate` 前算 depth + clamp + 回写 m_config。验证标准：clamp 后
wipe_tower_y=242.5 且无 TOOLPATH_OUTSIDE。升级 libslic3r 无效——核心从不
做此事。完整调研：`~/Downloads/擦塔越界调研报告-2c242c10f0e4b521.md`。

### 用户侧绕行（2026-09-21 实测）

通过 Orca「**导入**」方式（非打开工程）加载本 fixture 再另存：导入逻辑
**忽略 3MF 内的 wipe_tower_x/y，自动生成新塔位**（实测 165/244.697 →
13/214.5），重存文件引擎切片 **exit 0**，1/1 盘成功、零 error、正常产出
gcode.3mf（38MB）。即 P-08 的用户侧解法 = 导入重存，无需手动摆塔；
引擎对原文件报 exit 7 的拦截行为本身正确。

---

## P-26: 3MF 存储塔位与对象重叠 → TOOLPATH_CONFLICT z=0（未修复，检测行为正确）

**状态：** 2026-10-19 定位并完成隔离实验；**未修复**（用户侧绕行已双重
验证）。引擎的冲突**检测**本身正确——错在引擎不做桌面 GUI 的自动重摆塔。
**Fixture：** ACC-01 AMS 四色件（`~/Downloads/ACC-01__30-0.4NOZZLE_AMS_
4COLORS_Nightpetal[Framed]-U1-speed 4-1h1m...3mf` 及其 `修复-` 前缀版，
均未入库）。

### 现象

原版引擎 rc=7，TOOLPATH_CONFLICT "Wipe Tower 与 Assembly 对象" 第 0/9 层
z=0；桌面切片成功（文件名自带桌面判定：AMS 四色统计正确无严重警告）。

### 根因

3MF 保存的塔位 (15,109.032) 塔体 x∈[15,45] 再加裙边 ≈50，与 Assembly
对象左缘 x=44.21 重叠 5.7×28.3mm（第 0 层裙边尤甚）。桌面成功是因为
GUI 在渲染/保存时重摆塔位（与 P-08 同一机制面）；引擎直接消费存储塔位
（Print.cpp:1498），src/ 零处引用、零重定位。

### 验证证据（2026-10-19）

- 副本仅移塔 y→20 → **rc=0**，四色统计/59:56 时长全达标（引擎侧实验）。
- 用户修复版（模型右移 20mm 拉开间隙，塔位不动，桌面重存）引擎实测
  **rc=0**：4 色全解析（#FFFFFF/#000000/#0000A0/#FF9191），gcode.3mf
  4.5MB 正常产出，issues 仅 warning 级（预设替换 + 几何自交/多组件）。

### 判别法（进支持排障流程）

TOOLPATH_CONFLICT 消息里带 "Wipe Tower" 与对象名且 `z = 0.000000` →
先查 3MF 塔位与对象 bbox 重叠（含裙边量），重叠即用户文件问题/引擎缺
自动摆塔，**再**谈引擎 bug。

### 修复方向（未实施）

与 P-08 共用修复面：`process_plate` 前做塔位合法性处理（clamp + 对象
重叠规避/自动重摆）。P-08 的教训适用——延展量必须含 brim + cone base，
不能只用 depth。

---

## P-27: V-FM01b 双 bug 定稿——enable_filament_ramming 透传致塔深 +2mm + 检测器 0.137mm brim 亲吻分叉

**状态：** 2026-10-19 晚定稿。分离出**两个独立 parity bug**，各自定级：
Bug A 根因钉死（单键注入实验）；Bug B 触发器实证、桌面侧差异未 diff（无 2.4.0 源码）。
**Fixture：** V-FM01b Sunflower（线上 5215，3 盘全彩；原版 + `修复-` 桌面重存版在
`~/Downloads/`，单键补丁版在 /tmp，均未入库）。

### 现象

- 原版与修复版引擎 rc=7 完全同症：盘 3 TOOLPATH_CONFLICT "Wipe Tower <->
  Sunflower O_.step" 第 0/30 层 z=0；盘 1/2 成功但只解析 3/4 色
  （#FFFFFF 槽不现——独立缺口，见文末）。
- 桌面参照（Sunflower A__PLA_2h16m.gcode.3mf）：3 盘全成、4 色全解析。
- 不崩溃（fe5c61c；该件 flush 矩阵实为 **8×8=64 值** vs 4 槽）。

### 前置证伪：塔位假说（2026-10-19 上午）

"原文件塔位为单值"记录有误——原版 project_settings.config 本就带逐盘 3 元素
数组 x=[206,202.571,206]、y=[215.286,209.862,208.857]，修复版逐值相同；
`Config.hpp:435 get_at(i)` 0 基逐盘取值 → 塔位 parity 成立，塔位不是分叉源。

### Bug A：塔深三盘恒 +2.00mm ← enable_filament_ramming 透传（根因钉死）

取证路径（ORCA_KEEP_FAILED_GCODE，ccede9e）：保留引擎三盘 gcode（含失败盘），
解包桌面 gcode.3mf 逐盘参照。

- **实测**：塔体后缘（y=208.857）、宽度 35、位置三侧全同；**前缘三盘恒
  +2.00mm**（228.49/226.49、223.06/221.06、222.06/220.06）——系统性。
  盘 3 CP EMPTY GRID 引擎 9 行 vs 桌面 7 行（0.5mm 行距）恰 2.0mm。
- **对象侧无分叉**：Sunflower O_.step 第 0 层 433 挤出点逐点相同（含 brim，
  y_max=205.794）。
- **gcode 头三方配置 diff**：引擎两次均用**内嵌脏打印机预设 MyToolChanger**
  （桌面=官方 Snapmaker U1）；原版-引擎带 8 槽向量族 +
  `enable_filament_ramming=1`、`machine_tool_change_time=0`、
  `tool_change_temprature_wait=1`、`z_hop_when_prime=1`、
  `extruder_clearance_radius=40`；修复版-引擎（桌面重存已规范化）全为
  官方值（ramming=0、4 槽等）→ 塔 parity（220.057）。
- **单键注入二分**（给原版 project_settings.config 注入单键后重切量塔前缘）：
  - `purge_in_prime_tower=0` → 仍 222.057（**证伪**，/tmp/vfm01b_purge0.3mf）
  - `enable_filament_ramming=0` → **220.057，与桌面逐 mm 一致（坐实）**，
    /tmp/vfm01b_ram0.3mf
- **机制**：文件无该键 → 内嵌 MyToolChanger 预设缺省 1 被引擎透传 →
  WipeTower2 `m_enable_filament_ramming=1`，其 ungated 路径
  （WipeTower2.cpp:2204 等，注意 2107/2112/2125/2194 多处与 SEMM 门混合）
  向塔计入 ramming 体积 → 多 2 个 purge 行。桌面加载按官方 U1 规范化为 0。
- **方向性**：+2mm 朝床前（对象在塔后方 y≤205.794），**不是冲突触发器**；
  风险面=塔前伸挤占床前空间/诱发 TOOLPATH_OUTSIDE（P-08 族）。
- 修复方向：引擎加载对非 SEMM 打印机把 enable_filament_ramming 压 0
  （或内嵌脏预设键族按官方预设回填——与 MyToolChanger 脏 ID 历史问题
  合流，见 nvt-reset 判例 P-17 时期的云审查根因链）。

### Bug B：rc=7 触发器 = 0.137mm brim 亲吻，检测器行为分叉（桌面侧未 diff）

- **物理交叠双侧相同**：Sunflower O_.step 第 0 层底面 **12 条线段**穿越塔
  brim 后缘线 y=205.657（=塔后缘 208.857 − brim 3.2），最深 205.794，
  越线 **0.137mm**；引擎与桌面 gcode 逐点一致。
- **引擎报、桌面不报**：fork 2.4.1 检测器塔线集=outer_wall **含 brim**
  （WipeTower2.hpp:308 注释明言 "for wipe tower out wall and brim"），
  判据=线段真相交且交点距线段端点 >0.01mm（ConflictChecker.cpp:283-305）
  → 12 处命中 → rc=7。桌面 2.4.0 同几何零告警 → 检测器输入集/阈值/行为
  在 2.4.0→2.4.1 间漂移；**无桌面 2.4.0 源码，具体差异未定**。
- **独立性实证**：ramming0 补丁版（塔几何已 parity 220.057）仍同消息失败
  → B 独立于 A。
- 判定链（引擎侧）：Print.cpp:2763 门（!m_no_check && 无自适应层高）→
  ConflictChecker::find_inter_of_lines_in_diff_objs → 塔线集
  getTrueExtrusionLayersFromWipeTower（Print.cpp:5076，真实 outer_wall，
  pos=get_at(m_plate_index)），对象线集=perimeters/fills+support 按首实例
  shift 平移；锥底排除（30° 缺省但塔高 ~6mm → r≈1.6mm 够不到对象）。
- 用户侧绕行：对象与塔拉开 ≥brim 宽度量级间隙（同 P-26 ACC-01 的
  模型右移方案）。

### 已证伪推测汇总（防重走弯路）

1. "原文件塔位单值、引擎单值用于所有盘"——文件本带逐盘数组。
2. "塔几何尺寸/8×8→4 规整放大塔体积"——矩阵三方同为 4×4 同值
   （引擎规整=桌面=取 8×8 左上块），且 SEMM=0 下矩阵本就不参与。
3. "purge_in_prime_tower=1 致 +2mm"——单键注入证伪。
4. "+2mm 前缘是冲突触发器"——前缘朝床前，对象在塔后方；ramming0 后
   仍冲突。

### #FFFFFF 槽不现（独立缺口，未查）

盘 1/2 仅解析 3 色（#000000/#400000/#FFFF80），桌面 4 色全解析。
修复版（SEMM=0、矩阵清零路径）下依旧 3 色 → 与 flush 矩阵/SEMM 无关，
另有其因。原版-引擎 `filament_multi_colors=""` vs 修复版/桌面带全 4 色
值——线索之一，未跟进。

### 取证文件（重启即失，及时转存）

- 引擎 gcode：`/tmp/claude-1000/orca_slice_5b67-…/plate_{0,1,2}.gcode`（原版）、
  `orca_slice_b447-…`（修复版）、`orca_slice_69a6-…`（purge0）、最新目录（ram0）
- 桌面盘 3 参照：`/tmp/desk_plate3.gcode`（plate_1/2 在 /tmp/deskref/Metadata/）
- 单键补丁件：`/tmp/vfm01b_purge0.3mf`、`/tmp/vfm01b_ram0.3mf`
- 旧副本：`/tmp/vfm01b_towerfix.3mf`（"打进桌面塔位仍撞"是必然——原文件
  本就带逐盘塔位，该补丁冗余）

---
## 注意

引擎曾有临时调试改动（ORCA_KEEP_FAILED_GCODE，SliceEngine.cpp 两处）
当时未提交未还原——接手时先检查。

---

## W2 批次新增两例（2026-10-21，nightpetal / 5215）

QA W2 批次两用例在 2.2.0 报 rc=7 TOOLPATH_CONFLICT（Wipe Tower vs 对象
z=0），QA 02.01.06 与桌面 2.4.1 均 rc=0。实测归入本判例族：

| 用例 | 塔实际 bbox（引擎） | 对象区 | 交叠 |
|---|---|---|---|
| Nightpetal（盘1） | X[19.8,38.3] Y[113.9,132.6] | X[27,226] 起 | 11.3×18.7mm 真实交叠 |
| 5215（盘3） | — | Sunflower 族 | 同形态 |

- 塔位/塔宽/矩阵参数两侧一致（15/109.032/30）；**桌面塔更大**
  （X[10.1,48.1]，宽 38mm）且与对象交叠更深，桌面**不报**（无路径冲突检测）
- P-31 修复（ramming 冒充消除，塔深 −2mm）后两例**仍 rc=7**——再证
  "ramming/塔深非触发器"（V-FM01b 判例证伪 #4 的独立复核）
- 桌面塔宽 38mm vs 引擎 18.5mm 的展开差是独立 parity 缺口（flush 量→塔宽
  计算路径），未立案

### 决策选项（待产品，2026-10-21 呈现）

- **A. 接受现状**：引擎检测正确发现真实交叠（塔与对象路径重叠物理上会撞），
  rc=7 是正确安全拦截；桌面不检测是桌面宽松。成本：QA 修用例塔位
  （V-FM01b 同款右移 20mm 实测 rc=0）
- **B. 移植桌面 clamp**：塔位侵入对象时引擎自动移塔。P-08 尝试已失败一次
  （stash P-08 WIP：预测 depth 19.5 vs 实际 footprint 26.5 不匹配，塔
  footprint 计算需先解决）；且桌面 clamp 发生在 GUI 布局层，无头移植语义
  待定
- **C. 放宽检测容差**：对塔-对象交叠设阈值（如 <0.5mm 忽略）。风险：真
  交叠漏报；与 P-26 的 0.137mm brim 亲吻判例（桌面不报引擎报）同属容差
  标定问题，需一起定

---

## 10-21 批次报告新增三例（2026-10-22 分诊，HEAD @ e92c7da 复跑）

测试组 10-21 批次报告（run `20261021-075822`，跑在 10-21 四笔修复之前）
三个多色模型 SKIP，HEAD 复跑全部 rc=7 稳定复现，归入本族并补充两种**新
触发几何**：

| 用例 | 塔位（3MF 内嵌） | 触发几何 | 越界量 | 结果 |
|---|---|---|---|---|
| 机械螃蟹（盘2） | (28.28, 223.365) | 塔深 + 5mm brim → 塔区顶达 y=272.727 > 271 | 1.73mm | OUTSIDE, rc=7 |
| 索尼克 | (13, 6) | **塔身在床内**，auto_brim 5mm 围塔外扩，brim 轮廓前缘 y=0.987 < 1.0 | 0.013mm | OUTSIDE, rc=7 |
| 霜花龙 | (13, 68.5) | 塔与对象 bbox 交叠（Wipe Tower <-> Assembly, z=0） | — | CONFLICT, rc=7 |

- 三件均为 U1 工程（printable_area `[0.5,1]-[270.5,271]`，0.5/1.0 边距
  长期如此非近期演进；BedEpsilon=3×EPSILON≈3e-6，不构成实际容差）
- **索尼克是"塔位合法、仅塔 brim 出界"的首个实测例**：对象在床中央
  （y∈[109,163]），塔 (13,6) 完全在床内，但 `brim_width=5`（auto_brim；
  官方工艺预设不带该键，工程值 5 替换后存活且输出 3MF 同值，实测围塔
  生效）围塔外扩后前缘 0.987 < 1.0。取证：`--keep-failed-gcode3mf`
  解包后 `plate_1.gcode:2019` `G1 X56.814 Y0.987 E0.0836`（围塔 brim
  封闭轮廓线段）；起始线 `G1 X185 E15 F360` 是 erCustom 不计入检测
- **机械螃蟹盘2**：`plate_2.gcode:40187` `G1 X33.548 Y272.727 E0.0628`，
  塔 x 范围内的 brim 轮廓段；越界 1.73mm（P-08 原判例 0.21mm 的同型放大）
- **霜花龙**：CONFLICT 形态与 nightpetal/5215 相同。报告标"静默失败
  （无 error 级 issues）"是**工具缺口**：TOOLPATH_CONFLICT 是
  serious_warning 级，工具按 `level==error` 过滤找不到；引擎 JSON
  `exit_code=7 / error_code='TOOLPATH_CONFLICT'` 恒有值，契约未破

### 对决策选项的增量证据（2026-10-22）

- **选项 B 的门槛再抬高一档：clamp 必须 brim-aware**。索尼克证明塔位本身
  合法不代表安全——塔 brim 外扩可越过床边距线。与 P-08 段 2026-09-09
  调研发现（延展量 = depth + brim + cone base，19.5 vs 26.5）互为印证，
  移植前塔 footprint 计算问题（选项 B 已列的前置）现在还要加上"合法塔位
  也可能因 brim 出界"的 clamp 触发条件本身
- 选项 C 若只对塔-对象交叠设阈值，不覆盖 OUTSIDE 两例——OUTSIDE/CONFLICT
  两症状的容差/修复面需分别定

桌面侧三例的实际切片产物未取得（报告 SKIP 行不含桌面数据），"桌面确实
rc=0"未闭环，已向测试组请求基线产物对勘。
