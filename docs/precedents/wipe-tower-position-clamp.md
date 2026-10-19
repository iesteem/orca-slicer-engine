# P-08 / P-26 / P-27: 擦塔判例族（位置 clamp 缺失 / 塔位重叠 CONFLICT / 塔几何分叉疑案）

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
> | P-27 | 塔位 parity 已证仍 CONFLICT → 塔深度三盘恒 +2mm（flush 规整嫌疑） | 根因待钉死（gcode 级实锤） |

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

## P-27: 塔位 parity 已证仍 CONFLICT → 塔深度恒 +2mm 系统性分叉（根因待钉死）

**状态：** 2026-10-19 下午取证升级——塔位假说证伪、**塔深度分叉已 gcode 级实锤**
（每盘前缘恒 +2.00mm）；flush 体积规整偏大为头号嫌疑，最终行级交点待 fork 侧插桩。
**Fixture：** V-FM01b Sunflower（线上 5215，3 盘全彩；原版 + `修复-` 桌面重存版均在
`~/Downloads/`，未入库）。

### 现象（原版与修复版完全同症）

- 盘 3 rc=7 TOOLPATH_CONFLICT "Wipe Tower <-> Sunflower O_.step"
  第 0/30 层 z=0；盘 1/2 成功但只解析 3/4 色
  （#000000/#400000/#FFFF80，**#FFFFFF 槽在两盘都不出现**）。
- 桌面参照（Sunflower A__PLA_2h16m.gcode.3mf）：3 盘全部成功、4 色全
  解析。→ 引擎另有**第 4 色槽不出现**的独立 parity 缺口（未定位）。
- 不崩溃（fe5c61c 撑住；该件 flush 矩阵实为 **8×8=64 值** vs 4 槽，比
  P-02 判例的 3×3 更极端）。

### 已证伪：塔位分叉假说（2026-10-19 上午修正）

此前记录"原文件塔位为单值 (206,215.286)、引擎把单值用于所有盘"
**与实物不符**。实测（python zipfile 逐处核对，两文件各仅 1 处
wipe_tower_x/y）：

- 原版 project_settings.config：逐盘 3 元素数组 x=[206, 202.571, 206]、
  y=[215.286, 209.862, 208.857]（桌面保存时逐盘回写实锤）。
- `修复-` 桌面重存版：数组**逐值相同**。
- libslic3r 消费端 `Config.hpp:435 get_at(i)`：i 在界内取
  `values[i]`（0 基逐盘），越界回落 `front()`。3 盘 + 3 元素 → 引擎
  盘 3 塔位 = (206, 208.857)，**与桌面一致**。
- 决定性实验：桌面重存修复版引擎实测 **rc=7，冲突消息与原版逐字相同**，
  盘 1/2 仍 3 色 → 桌面重存不治，塔位不是分叉源。

### 塔深度分叉：gcode 级实锤（2026-10-19 下午，ORCA_KEEP_FAILED_GCODE 取证）

工具：重引入 `ORCA_KEEP_FAILED_GCODE=1`（Types.hpp TempFileGuard 单点守卫，
保留全部 plate_N.gcode 并打印路径）；桌面参照解包
Sunflower A__PLA_2h16m.gcode.3mf 的 Metadata/plate_N.gcode。

逐盘第 0 层 `;TYPE:Prime tower` 段实测（引擎失败 gcode vs 桌面成功 gcode）：

| 盘 | 塔体 y 范围 引擎 | 桌面 | 前缘差 |
|---|---|---|---|
| 1（引擎成功） | …[163.08, 228.49] | …226.49 | **+2.00** |
| 2（引擎成功） | …[79.49, 223.06] | …221.06 | **+2.00** |
| 3（引擎失败） | [208.857, 218.857] | [208.857, 216.857] | **+2.00** |

- **后缘（y_min）、宽度（35）、位置全部一致；前缘三盘恒 +2.00mm——系统性，非盘 3 特有**。
- 盘 3 CP EMPTY GRID：引擎 9 行（209.857→218.357）vs 桌面 7 行（209.357→215.857）
  ——恰好多 2 行 × 0.5mm 行距 = 2.0mm。塔深度按行量化，引擎比桌面**多算两个 purge 行**。
- 对象侧无分叉：Sunflower O_.step 第 0 层挤出 433 点**逐点相同**（含 brim，
  y_max=205.794 两侧一致）。分叉纯在塔侧。
- 次要漂移（2.4.1-base vs 桌面 2.4.0）：引擎 gcode 无
  `; WIPE_TOWER_BRIM_START/END` 标记；prime 起角相反（引擎左上 vs 桌面右下）；
  塔体宽度方向 brim 圈数循环结构同但无标记包裹。疑似同源代码漂移，未单独定级。

### 冲突判定链（fork 侧代码定位，2026-10-19）

- 检测入口：`Print.cpp:2763-2781` `_make_skirt` 后、`!m_no_check &&
  !has_adaptive_layer_height` 时 `ConflictChecker::find_inter_of_lines_in_diff_objs`。
- 塔侧线集：`FakeWipeTower::getTrueExtrusionLayersFromWipeTower`
  （Print.cpp:5076）= 真实 `WipeTower2::m_outer_wall` 逐层多边形（Print.cpp:3533
  喂数），pos 经 `get_at(m_plate_index)`（Print.cpp:2769）——塔深度分叉直接进检测器。
- 对象侧线集：每 PrintObject 的 perimeters+fills+support_fills，按
  `instances().front().shift` 平移（ConflictChecker.cpp:233-234）——注意多实例对象
  仅用第一实例 shift，若引擎路径坐标空间与桌面不同会放大/缩小重叠。
- 判据：线段真相交且交点到两线段端点最小距离 > 0.01mm（双 support 为 100mm，
  ConflictChecker.cpp:283-305）；1mm 网格光栅化加速。
- 锥底（cone_angle 缺省 30°）在本件塔高 ~6mm 下 r≈1.6mm，够不到对象——已排除。
- 引擎侧消费：SliceEngine.cpp do_postprocessing 读 `conflict_result` 报
  serious_warning + rc=7（引擎检测行为正确，错在塔几何本身偏大）。

### 根因嫌疑与下一步

头号嫌疑：**flush_volumes_matrix 规整后体积偏大**——8×8 矩阵 vs 4 槽，
fe5c61c 移植的 PresetBundle 规整路径（引擎侧）与桌面 2.4.0 的规整结果不同，
多出的 purge 体积量化成 +2 行。次嫌疑：fork 2.4.1-base vs 桌面 2.4.0 的
WipeTower2 深度公式漂移。**#FFFFFF 槽不现**（盘 1/2 仅 3 色）可能与矩阵规整
同根（槽位参与 toolchange 的映射），未验证。

1. 引擎侧加日志：规整后打印 wipe volumes 矩阵 + toolchanges（对比桌面
   project_settings 内矩阵推算的有效体积），定位哪对槽位的体积被放大。
2. fork 侧插桩 ConflictChecker：dump z=0 塔/对象线集，找出确切相交线段
   （区分"塔外墙 2mm 前伸压线" vs "对象 shift 坐标空间错位"）。
3. #FFFFFF 缺口单独开查（plates 1/2 成功 gcode 已保留）。

### 取证文件（重启即失，及时转存）

- 引擎三盘 gcode：`/tmp/claude-1000/orca_slice_5b67-7f20-1d5f-adbc/plate_{0,1,2}.gcode`
- 桌面盘 3 参照：`/tmp/desk_plate3.gcode`（plate_1/2 在 /tmp/deskref/Metadata/）
- 旧副本：`/tmp/vfm01b_towerfix.3mf`（重打需修 count=1；"打进桌面塔位仍撞"
  如今看是必然——原文件本就带逐盘塔位，该补丁是冗余操作）

---

## 注意

引擎曾有临时调试改动（ORCA_KEEP_FAILED_GCODE，SliceEngine.cpp 两处）
当时未提交未还原——接手时先检查。
