# P-30: 工艺预设替换把 3MF 塔旋转角 270° 冲成默认 0° → 塔转向贴边/越界（已修复）

> **状态：** 根因定稿（2026-10-20 取证），**已实施方向 B（B1+g 门控豁免，
> 2026-10-21）**——产品决策选定替换豁免；`kProjectOwnedProcessKeys` 4 键
> （rotation + dithering 三键）在官方值==代码默认值（normalize backfill
> 工件）时豁免覆盖，官方显式声明值（fdm_process_idex 的 90）仍生效。
> 测试：test_preset_substitution（值存活 + tripwire）+ test_high_flow
> （端到端回写）。
> 塔族既有三例（P-08 clamp 缺失 / P-26 塔位重叠 / P-27 ramming 透传）与本例
> **机制不同**：前三例是"引擎缺少桌面的塔位重算"，本例是"官方预设 wholesale
> 覆盖机制对预设未携带键按默认值兜底，清零了项目级配置"。

---

## TL;DR

HueForge 等第三方导出的 3MF 用**塔旋转角**（`wipe_tower_rotation_angle=270`）
控制擦塔从锚点向床内伸展。引擎的官方工艺预设替换把该键**无条件覆盖为 0**，
塔转向后整体向热床后缘平移约 30mm 并越出可打印区 4.7mm。U1 官方工艺预设
**不携带该键是工艺团队有意删除**（2026-10-20 确认），全部 112 个官方工艺
预设仅 IDEX 一例保留声明（值 90）；覆盖生效的机制是 wholesale 替换对预设
未携带键按 `FullPrintConfig` 默认值（0）兜底。"预设不带"的意图边界——
"不主动设置"是否等于"应清零 3MF 项目值"——是本案的**产品决策点**。

## 1. 现象与复现

**触发文件：** `~/Downloads/2859114a0ed451d7.3mf`（HueForge 导出，
U1 单对象双色，270×270 床）

**观察方式：** 将原始 3MF 与切片产物 gcode.3mf 分别导入 Orca 2.4.1——
gcode.3mf 中擦塔明显比原项目更贴近热床后缘。

**本地复现（决定性）：**

```bash
build-static/orca-slice-engine ~/Downloads/2859114a0ed451d7.3mf \
    -o /tmp/repro -r package/resources -j /tmp/repro
# → rc=7 TOOLPATH_OUTSIDE
# 日志：Applying official process preset "0.08mm Standard @Snapmaker U1
#        (0.4 nozzle)" (0 user overrides preserved)
```

## 2. 取证数据（定量）

| | 源 3MF | gcode.3mf（引擎切） |
|---|---|---|
| 塔锚点 `wipe_tower_x` / `wipe_tower_y` | 20 / 244 | 20 / 244（**未变**） |
| `wipe_tower_rotation_angle` | **270** | **0**（gcode 头实证） |
| 塔实际占位 | X 14.97~43.53，Y 208.97~249.03 | X 14.99~51.81，**Y 238.99~275.71** |
| 床（printable_area） | 0.5×1 ~ 270.5×271 | 同 |

- 塔锚点**两边一致**——漂移的不是塔位坐标，是**旋转角丢失**。
- 270° 时塔从锚点 (20,244) 向 −Y（床内）伸展，完全在床内；
- 被重置 0° 后向 +Y 伸展，塔体 Y 达 275.71，**越出可打印上缘 271 达 4.7mm**。
- gcode 实测塔挤出坐标从 `WIPE_TOWER_START/END` 段提取（5596 条挤出移动）。

## 3. 根因链（五环，逐环实证）

```
3MF 写入 rotation=270
        │
        ▼
① 引擎加载 3MF → m_config.rotation = 270          （正确）
        │
        ▼
② apply_process_official_preset() 触发 wholesale 覆盖
   src/SliceEngine.cpp:1661
   overwrite_all_keys_from_except(m_config, official->config, user_overrides)
        │
        ├─ user_overrides 为空：源 3MF 无 different_settings_to_system
        │  （日志实证 "0 user overrides preserved"）
        │
        ▼
③ official->config 里的 rotation=0 从哪来？
   官方预设 JSON 不声明该键（见 §4）
   → Preset::normalize process 分支补齐缺失键：
     OrcaSlicer/src/libslic3r/Preset.cpp:446-452
     遍历 s_Preset_print_options（rotation 位列 Preset.cpp:983）
     缺失键用 FullPrintConfig 默认值 backfill
   → 默认 0 定义于 PrintConfig.cpp:6219
        │
        ▼
④ 覆盖生效：m_config.rotation 270 → 0
        │
        ▼
⑤ 塔按 0° 从锚点向 +Y 展开 → Y 238.99~275.71，越界 4.7mm
   → 本地重切 rc=7 TOOLPATH_OUTSIDE
   （用户线上那份 gcode.3mf 塔同样越界 4.7mm 但成功产出——
    不同时期产物，检测行为差异，不影响主结论）
```

## 4. 关键事实：U1 工艺预设不携带该键是工艺团队有意删除（2026-10-20 确认）

**引擎侧官方库**（`package/resources/profiles/Snapmaker/process/`，112 个工艺预设）：

| 检查对象 | 是否声明 `wipe_tower_rotation_angle` |
|---|---|
| 全库 112 个工艺预设 | 仅 `fdm_process_idex.json` 一例，值 **90**（IDEX 双头形态） |
| 本 3MF 命中：`0.08mm Standard @Snapmaker U1 (0.4 nozzle)` | 不声明 |
| └ `fdm_process_U1_0.08` | 不声明 |
|   └ `fdm_process_U1_common` | 不声明 |
|     └ `fdm_process_U1` | 不声明 |
| printer（machine）预设全部 | 不声明 |
| 桌面版完整库（OrcaSlicer/resources，同一套） | 同样仅 idex 一例 |

**定性（修正版）：** 该键从 U1 官方工艺预设中移除是**工艺团队手动删除的
决策**，非疏忽遗漏。但删除预设键与 wholesale 替换语义叠加后，实际效果从
"预设不主动设置塔角"放大为"清零 3MF 项目值"——覆盖用户 270 的 0 来自
`Preset::normalize`（`Preset.cpp:446`）的默认值兜底，而非任何预设 JSON
里的显式值。**"预设不携带"是否应当等于"清零用户值"，正是需要产品决策
的意图边界**；idex=90 保留声明说明该键在预设体系内仍属合法管辖域。

## 5. 桌面版为何不受影响

塔三键（`wipe_tower_x` / `wipe_tower_y` / `wipe_tower_rotation_angle`）
在桌面版属于**项目级选项** `s_project_options`
（`OrcaSlicer/src/libslic3r/PresetBundle.cpp:210-212`）：

- 桌面导入 3MF 时，塔三键走独立通道 `project_config.apply_only(config,
  s_project_options)`（`PresetBundle.cpp:3138`），**不参与** process/printer
  预设替换，270 存活到切片。
- 引擎无 GUI、无 project_config 通道，全部键平铺在 m_config，官方预设
  wholesale 覆盖把项目级键一并咬掉。

**塔 x/y 为何幸存而 rotation 中招：** x/y 不在 `s_Preset_print_options`
（预设键表）里，normalize 不会为它们 backfill，official->config 里压根
不存在 → 覆盖循环写不到它们。而 rotation **恰好跨两个列表**（既是项目级
键又是预设键表成员），normalize 给它补了默认 0 → 被覆盖循环写入。
同一 3MF 里三键两种命运，机制自洽。

## 6. 影响面

- **谁会中招：** 任何 `wipe_tower_rotation_angle ≠ 0` 的 3MF。旋转角非零
  的典型来源是第三方工具（HueForge 等）与用户在桌面手动转塔后存盘。
- **后果分级：** 塔转向后占位改变——轻则塔漂移贴边（视觉差异、与用户预期
  不符），重则越出可打印区（rc=7 TOOLPATH_OUTSIDE 拒切，或如本例线上
  产物带越界塔）。
- **与塔族前三例的叠加：** P-08（clamp 缺失）解决的是"塔位超床回推"，
  即使 clamp 移植完成，本例的旋转角重置仍会先把塔摆错方向——两缺口独立。

## 7. 决策结果与实现（2026-10-21 实施）

**决策：** 方向 B（替换豁免），范围 = 4 键全豁（用户 2026-10-21 确认）。

**实现（B1 豁免集 + 默认值门控 "B1+g"）：**
- `src/SliceEngine.cpp` `kProjectOwnedProcessKeys` 候选表（4 键）+
  `apply_process_official_preset` 内 `user_overrides.erase("post_process")`
  之后插入门控循环：仅当 `official->config` 的值 ==
  `FullPrintConfig::defaults()` 的值（即 normalize backfill 工件）时把键
  并入 user_overrides；官方显式声明的非默认值（fdm_process_idex 的 90）
  保持官方所有权。豁免发生时打 info 日志（计数不并入 "N user overrides
  preserved"，保持日志真实）。
- 门控是 Plan 阶段的关键设计提升：idex 预设**故意声明** rotation=90
  （`fdm_process_idex.json:39`，111/112 官方预设零声明中的唯一反例），
  无条件豁免会在 IDEX 路径把用户值凌驾于官方意图之上。
- 编译坑：const 对象（official->config / defaults()）上只能用单参
  `option(key)` 查找；两参 `(key, false)` 重载是非 const 的（会创建缺失键）。
- 测试锚点：test_preset_substitution.cpp
  "Project-owned process keys survive substitution (wipe tower rotation)"
  （值存活 + 门控前提 pin + layer_height 对照断言）、
  "Official process presets declare project-owned keys only in fdm_process_idex"
  （tripwire，防上游漂移）；test_high_flow.cpp
  "Wipe tower rotation angle survives process preset substitution"
  （端到端：gcode 头 echo + 打包 project_settings 回写）。
  fixture：`tests/fixtures/u1_preset_test_wipe_tower_rot.3mf`
  （基线改造，rotation=270 + dithering_local_z_mode=1，
  different_settings_to_system 原样不含 4 键——回归条件成立）。

**未采纳的备选：** B2 快照回植（nvt 先例的三条前提——向量键/长度博弈/
官方永不声明——本案均不满足）；工艺侧补键（与删除决策冲突，见下表保留
原决策讨论）。

**决策语境存档（2026-10-20 讨论，次日收束为方向 B）：**

| 方向 | 含义 | 对用户的影响 | 实现成本 |
|---|---|---|---|
| **A. 接受现状** | 维持官方预设不带塔角 + 替换按默认 0 兜底，云切片统一 0° 摆塔 | HueForge 等第三方文件塔位漂移/越界现象持续，由客服口径消化（见运营版文档） | 零改动；可选增强=塔位 clamp（P-08 方案）缓解越界拒切 |
| **B. 替换豁免** | 覆盖时对塔角例外放行，保留 3MF 项目值 | 塔位与用户预期一致；官方模板对云结果的管控多一个例外 | 小（两种实现见下） |

**方向 B 的两种实现（供工程评估，非产品决策项）：**

- **B1 项目级键豁免**：`s_project_options` 相关键并入 `user_overrides`
  保护集——对齐桌面 project_config 通道语义，一处改动；
- **B2 快照回植**：覆盖前快照、覆盖后回植——同 nozzle_volume_type 修复
  模式（`SliceEngine.cpp:1308` 先例，3640a49），局部好审，但逐键治标。

**工艺侧第三选项（不在引擎可控范围）：** 恢复被删除的键（在 U1 预设中
显式声明）。这等于推翻工艺团队当初的删除决策，且桌面用户改塔角存 3MF
后仍会被预设值覆盖——**与方向 B 语义冲突**，需工艺团队参与讨论时排除
或重新定义。

**回归前置（任一方向实施后）：** 须含本 fixture（塔 Y 回床内；若方向 A
则验证 clamp 缓解效果），并保留 rotation=0 主流路径不回归。

**家族史备注：** 这是"官方 wholesale 覆盖 + normalize 默认 backfill →
用户项目值丢失"家族第三例——第一例 brim_width（auto 模式归零，已有
slicing-copy 规避），第二例 nozzle_volume_type（已修，快照回植），本例
rotation。

**交集键实测（2026-10-20）：** `s_project_options`（29 键）∩
`s_Preset_print_options`（332 键）= **4 键**：

```
wipe_tower_rotation_angle   ← 本案
dithering_local_z_mode      ┐
dithering_local_z_whole_objects ├ 混色 dithering 三键，同一通道暴露，
dithering_local_z_infill    ┘ 影响面待独立评估（另案，不在本文定论）
```

即若决策为方向 B，豁免/回植范围圈定到这 4 键（或其中塔键）即可，工程
侧"范围难圈定"的顾虑实际很小。（决策已定：4 键全豁，dithering 三键的
影响面随门控豁免一并消解——保留用户值即桌面行为。）

## 8. 已排除的假说（防讨论重走弯路）

| 假说 | 证伪方式 |
|---|---|
| 塔位 x/y 被改写 | 两边 x/y 逐值对比，20/244 完全一致 |
| 引擎加载 3MF 时就丢了 270 | 覆盖发生在 apply_process_official_preset；gcode 头 `wipe_tower_x=20 / y=244` 证明加载通道正常 |
| 桌面 GUI clamp 回写差异（P-08 机制） | 本例塔锚点未动，非重定位问题；桌面无 GUI 干预也保留 270（project_config 通道） |
| 官方预设显式声明了 0 | 全库 grep + U1 继承链逐层解析，仅 idex 声明 90 |
| HueForge 写入的值不合法 | 桌面 2.4.1 导入同一文件塔位正确；值语义（deg）在 PrintConfig 定义明确 |

## 9. 取证清单

- 触发文件：`~/Downloads/2859114a0ed451d7.3mf` + `…gcode.3mf`（2026-10-20）
- **修复终态验证（2026-10-21，原始文件重切）**：rc 7→**0**；issues 仅
  3 个 SUBSTITUTED warning（TOOLPATH_OUTSIDE 消失）；gcode 头与 3mf 回写
  均带 rotation=**270**；塔实际挤出 Y **220.99~257.81**（修复前
  238.99~275.71 越界 4.7mm，现床内余量 13.2mm）；塔深与源 3MF 预览
  （Y 209~249）有 ~10mm 差异属正常——塔深由切片时 flush 矩阵实际计算，
  预览是 HueForge 端估计值。
- 本地复现：`/tmp/wt_repro/repro.json`（rc=7 TOOLPATH_OUTSIDE +
  PROCESS_SUBSTITUTED 0 overrides）、`repro.log`
- 解包对比：`/tmp/wt_2859_src/`、`/tmp/wt_2859_out/`（project_settings /
  plate_1.json / gcode 塔坐标提取脚本见本文 §2 注）
- 回归 fixture：`tests/fixtures/u1_preset_test_wipe_tower_rot.3mf`
  （2026-10-21 入库，python zipfile 整档重打包——档内有非 ASCII 条目，
  禁用 shell unzip/zip）
- 注：本文 §3 行号为 2026-10-20 取证时快照，SliceEngine.cpp 后续有漂移
  （apply_process_official_preset 1661→1672，覆盖调用 →:1734）；
  以函数名/语句锚点定位为准。
- 关联记忆：`wipe-tower-rotation-preset-overwrite`（含全部 file:line）
- 关联判例：P-08/P-26/P-27（`wipe-tower-position-clamp.md`）、
  云审查配置回写根因（nozzle_volume_type 同族）
- 遗留备忘：nvt 快照回植（SliceEngine.cpp，3640a49）是无条件回植——若
  未来官方 printer 预设开始声明 nozzle_volume_type 类键，将与本案
  idex=90 同理出错；本案门控模式是其将来的参照修复。
