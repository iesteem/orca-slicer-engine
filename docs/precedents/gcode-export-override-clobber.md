# G-code 导出用覆盖前快照冲掉耗材域 retract 覆盖 → U1 全床模型螺旋整圆扫出热床拒打

**状态：** 已修复（Up 侧 GCode.cpp 重排序 + 引擎侧告警有效值化，2026-10-23）
**Fixture：** `tests/fixtures/u1_spiral_zhop_repro.3mf`（云任务 2033 现场输入
原件入库，md5 `094551c8eca1a84892b6169563a42ac6`；与
`~/Downloads/7a6bf0803d68c312.3mf` 同字节，同工程副本
`切片通过------高低流量最大模型测试.3mf`，源工程「高低流量混用-最大模型-极小模型-异常摆放.3mf」）
**判例编号：** P-35

## 现象

U1（热床 X[0.5,270.5]×Y[1,271]）云任务 2033（SnapSpeed PLA ×4，270mm
全床空壳立方体，1350 层）引擎切片产物被固件拒打「X轴超出热床平面」；
桌面 2.4.1 同配置正常打印。引擎产物每个立方体盘 1344 条 `G3 … P1`
整圆（半径 = z_hop/(2π·tan(travel_slope°)) = 0.4/(2π·tan3°) = 1.2166mm），
几乎全部越界（Y 最深 273.139，X 一条 -0.548）。

**固件侧定案**（固件团队答复，2026-10-23）：打印前校验对**整个弧取点**
（非仅端点）、**无容差**、X/Y **无轴报告优先级**（先违规先报）。⇒ 弧上
任一采样点出床即拒打；全床模型外壁离边 0.21mm < r=1.217mm 必越界。

## 根因

**fork 提交 376524d9a6**（2026-05-27 "Fix wall speed stuck at defaults by
applying full print config to GCode generator"，+1 行）在
`GCode::_do_export` 中把顺序写反：

```cpp
this->apply_print_config(print.config());        // 覆盖后（耗材域已合并）← 先
m_config.apply(print.full_print_config(), true); // 覆盖前快照            ← 后，冲掉前者
```

`print.config()` 是 `Print::apply` 里 `m_config.apply(filament_overrides)`
（PrintApply.cpp，print_config_diffs 对 `extruder_retract_keys` 逐键做的
`apply_override` 合并）之后的**权威值**；`full_print_config()` 是合并**前**
的 `new_full_config` 快照。后者最后应用 ⇒ **整个 extruder_retract 覆盖键族**
（z_hop_types、retraction_length、retraction_speed、deretraction_speed、
wipe、travel_slope…）在运行时全部静默回落机器基值。U1 官方机器预设
`z_hop_types = Auto Lift ×4` → `GCode.cpp:7271` 换层 zhtAuto 强制
SpiralLift → 每层整圆。

桌面发布版行为正确（混合耗材受控对照：SnapSpeed/PLA-CF 槽 0 条螺旋、
Wood 槽 300/300 全螺旋 = 按槽生效），引擎全槽螺旋 = 覆盖被冲掉的铁证。
**波及面大于本案症状**：所有走耗材域 retract 覆盖的引擎切片（回抽长度/
速度等）自 2026-05-27 起运行时都被打回机器基值。

### 排查关键（回显陷阱）

引擎与桌面产物的 gcode 配置回显**逐键逐值相同**（`z_hop_types = Auto×4`、
`filament_z_hop_types = Slope×4`、`step_size=2,2,2,2`）——因为回显源是
`append_full_config` 读 `print.full_print_config()`（**覆盖前**快照），
与运行时读的 `print.config()`/gcodegen m_config 不是同一份。**对比回显
永远看不出此类回归，必须断言实际移动指令。**

## 修复

1. **Up 侧（GCode.cpp，+11/-1）**：重排序——先用快照补 FullPrintConfig
   独有键（保住 376524d9a6 的墙速意图），再让覆盖后的 `print.config()`
   在重叠键上取胜。
2. **引擎侧（SliceEngine.cpp）**：`check_spiral_lift_near_boundary` 从读
   机器平键改为**有效值**判定（clone + `apply_override` 组合
   `filament_z_hop_types`，镜像 PrintApply.cpp:260 的非 nil 门）——否则
   修复后机器基值 Auto 仍会触发 `SPIRAL_LIFT_NEAR_BOUNDARY` 误报
   （实测修复版 JSON 立即出现 plate 1/3 两条误报后由本项消除）。

## 验证证据

- **复现**：当前 HEAD（.a 2026-10-21 构建）实切现场 3mf，plate_1/plate_3
  各 1344 条 G3 P1（与 2033 产物逐数吻合），槽归属 T0/T2=立方体 extruder
- **插桩链**（临时日志重编两 .o，P35A-F 六点）：diff/合并/落地全对
  （`overriden_by=true`、merged=Slope×8 进 print.config()、2614 拷入
  gcodegen=Slope×8），**唯一断点 = 2615 快照应用后回落 Auto×4**
- **绿证**：修复后四盘 G3/G2 = **0**，rc=0；与桌面 From_Udisk 在同一
  失效位置（喷嘴 (1.946,268.951) 换层段）**逐行一致**（含 wipe 移动与注释）
- **红证（M2）**：还原 Up 修复跑 `[zhop]` 锚点 → 红（`arcs=4 spirals=4`
  于首个迭代码盘；产物全量分布 4+4+1344+1344）；恢复后复绿（23 断言）
- **回归族**：`[preset]` 8 例、`[highflow]` 3 例（376524d9a6 墙速意图的
  速度断言仍在）、`[pathconflict]` 6 例（spiral_lift_near_boundary 用例
  有效值仍含 Auto，告警保留）全绿；**全量 231 用例全绿**（基线 230 +
  新增 [zhop]，2026-10-23，build-static/tests junit 归档）

## 已证伪的推测

1. **「引擎丢耗材域 flow-variant 覆盖（overwriteExtruderFrom skip 是
   直接根因）」**（10-23 首诊 + 调查报告 §3 结论；10-23 晚外部复核亦持
   此论——纯代码静态阅读，无证据 gcode/现场 3mf）：找回的 2033 现场 3mf
   携带完整正确的组合值（`filament_z_hop_types=Slope×8` +
   `step_size=[2,2,2,2]` + 交错 flow_support），**透传全链存活到导出**
   （回显折叠行即证）；skip 只是让替换路径不写这些键，3MF 透传绕过了它。
   按 v1 方案补「分段展开 composer」修不了本案（产出同样值，照样被 2615
   冲掉）。composer 作为 P-15 语义完备性议题（官方预设值替换 3MF 陈旧
   组合值）另案决策——见下方「残留分歧（S2）」。
2. **「gcode 回显可当运行时证据」**：回显=覆盖前快照（见根因节）。
3. **「桌面没有换层强制螺旋」**（10-22 假说，10-23 受控实验推翻）：桌面
   同样强制（GCode.cpp:7271 共有），只是 Wood 槽（全库唯一 Auto 承载者）
   之外都被 Slope 覆盖关掉。
4. **「10-16 产物系陈旧构建」**：当前 HEAD 活体复现，排除。

## 残留分歧（S2 后续项，2026-10-23 外部复核确认）

`overwriteExtruderFrom` skip 仍是真实缺口，但作用于**另一类输入**：3MF
携带 nil/旧值 flow-variant 键（旧版桌面导出、转换工具、App 生成文件）。
桌面 `PresetCollection::load_external_preset`（Preset.cpp:2121-2150）会把
不在 different_settings_to_system 里的键恢复成系统预设值（已核验），引擎
无对应刷新 → 此类文件修复 P-35 后仍与桌面分歧（nil→机器 front=Auto→
螺旋，桌面 Slope）。补法：在 overwriteExtruderFrom 内加逐槽分段感知写入
（`OverwriteFlowVariantSegment`——旧/新段长重排、段内不足回退预设第 0 值、
**只写 filament_\* 键不碰 z_hop_types**，覆盖交给 Print::apply——外部复核
提供的设计骨架）；配套回归测试：全槽 nil 的 3mf 替换后断言
`get_value_at(..., ConfigFlowDomain::Filament, i)` == Slope。属 P-15
wholesale 语义完备性，行为变化面（其余 18 个 flow 键改用官方值）需 PR 明示。

另：螺旋半径公式中 travel_slope=3 为**度数**，运算时须换算弧度
（r = hop/(2π·tan(3°))）；本判例历史文本中 "arctan(3°)" 为数学简写，
数值 1.2166mm 与实测 1.217 吻合。

## 排查方法备忘

- **无符号二进制的取值取证**：release 无 -g 时，给 Up 树打临时插桩
  （BOOST_LOG + 空安全 dump）→ `cmake --build build-engine --target
  libslic3r -- -j 2`（make 自动重归档 .a）→ 重链引擎 → 还原插桩再重编。
  比重建 debug 库快一个量级。
- **coEnums 潜在雷区（87fa 同族，本案非根因，勿混）**：
  `ConfigOptionEnumsGenericTempl` 的 vector 构造器不设 `keys_map`
  （Config.hpp:1680-1681），initializer_list 构造器更是 `keys_map(keys_map)`
  **自初始化**（:1679）；`serialize_single_value` 对 `*keys_map` 无空检查
  （:1766）。实测 Print 静态 FullPrintConfig 成员 `z_hop_types` 的
  keys_map=NULL，直接 `vserialize()` 即 SEGV（插桩首版崩溃实证）。
  上游硬化候选：构造器统一带 keys_map + serialize 空守卫。
- **zip 条目顺序陷阱**：gcode.3mf 内 `.gcode` 条目的 namelist 顺序是
  plate_2,4,1,3（写入序非编号序），逐盘断言先撞哪盘要看插入序。
- **G0/G1 端点扫描看不到弧**；`P1` 整圆（端点=起点）按 360° 计；
  `grep '^G2'` 会误中 `G21`。
