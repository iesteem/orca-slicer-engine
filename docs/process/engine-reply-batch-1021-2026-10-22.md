# 引擎组答复 · 10-21 批次报告（23 用例，2026-10-22）

> 复验环境：本地 2.2.0 @ `e92c7da`（含 10-21 全部四笔修复：P-29/P-30/P-31/P-32），
> 资源 02.03.04.04。复现命令（CLI 形式，与服务器直连等价）：
> `orca-slice-engine <模型.3mf> -o /tmp/out/<名> -r package/resources -j /tmp/out/<名>`
>
> 结论速览：**1 FAIL + 4 SKIP + 1 NO_RESULT 中没有当前可修的引擎缺陷**。
> ceshi 的 4 项 FAIL = 基线 schema/资源版本错位（值全部一致）；三个多色模型
> （机械螃蟹/索尼克/霜花龙）全部归宗已知**塔族判例**（P-08/P-26/P-27，待产品
> 决策，见 `docs/precedents/wipe-tower-position-clamp.md`）；Cyber Fish 是正确
> 拒绝；Faceted_Bowl_2 是跑批落位问题。12 WARN 的 SUBSTITUTED 归因全部成立。
>
> 备注：报告 run `20261021-075822`（07:58）早于当天全部修复（09:47 起），但
> 上述 FAIL/SKIP 四例在含修复的 HEAD 上**原样复现**——与那批修复目标无交集。

---

## 答FAIL：ceshi 4 项参数透传 —— **基线版本错位，非引擎缺陷**

逐项实测（引擎 HEAD 输出 3MF 的 `project_settings.config`）：

| 检查项 | 基线 | 引擎 | 定性 |
|---|---|---|---|
| bridge_flow | '0.8' | '1' | 官方资源演进，见下 ① |
| initial_layer_infill_speed | '105' | ['105','105'] | fork #807 向量化，值一致，见下 ② |
| support_speed | '150' | ['150','150'] | 同 ② |
| enable_overhang_speed | '1' | ['1','1'] | 同 ② |

**① bridge_flow 0.8→1：** 与 W2 二批问题一同一件事——官方资源库 PR #937
（2026-09-30）将该键 0.8→1，引擎跟随现行官方资源（02.03.04.04），你们桌面
基线二进制捆绑的是旧资源。ceshi.3mf 内嵌工艺为旧命名 `0.20 Standard @Snapmaker U1`，
引擎按云安全策略替换为现行官方 `0.20mm Standard`，bridge_flow 取到现行值 1。
引擎 JSON 里有完整 `PROCESS_SUBSTITUTED` 归因记录。

**② 三项"标量 vs 向量"：值一字不差，是 schema 演进。** fork PR #807
（2026-09-01，"Flow back release_2_3_7 fixes into main"）将
`initial_layer_infill_speed`、`support_speed` 改为 coFloats、`enable_overhang_speed`
改为 coBools（per-extruder 向量键，PrintConfig.cpp:2893 / 5644 / 1255）。
引擎按现行 schema 序列化为向量；基线桌面二进制是旧 schema（标量）。

**建议（工具侧两改）：**
1. 参数比对前做**形状归一**：基线标量 vs 引擎向量时，若向量所有元素相等且
   等于基线值 → PASS；
2. 基线桌面/资源对齐版本 02.03.04.04（与 W2 二批请求清单第 3 项同一件事）。

---

## 答SKIP×3：机械螃蟹 / 索尼克 / 霜花龙 —— **全部塔族（P-08/P-26/P-27），HEAD 稳定复现 rc=7**

三例的共同背景：3MF 内嵌擦塔坐标，桌面 GUI 加载时会重算塔位并**回写**，
引擎无 GUI 直接消费存储值。该判例族已合订于
`docs/precedents/wipe-tower-position-clamp.md`，修复方向待产品 A/B/C 决策
（与 W2 批次 nightpetal/5215 同桶）。本轮三例补充了两种**新的触发几何**：

| 用例 | 塔位 | 触发几何 | 越界量 | 结果 |
|---|---|---|---|---|
| 机械螃蟹（盘2） | (28.28, 223.365) | 塔深 + 5mm brim → 顶达 y=272.727 > 271 | **1.73mm** | OUTSIDE, rc=7 |
| 索尼克 | (13, 6) | **塔身在床内**，auto_brim 5mm 围塔外扩，brim 轮廓前缘 y=0.987 < 1.0 | **0.013mm** | OUTSIDE, rc=7 |
| 霜花龙 | (13, 68.5) | 塔与对象 bbox 交叠（Wipe Tower <-> Assembly, z=0） | — | CONFLICT, rc=7 |

**取证方法**（你们可复验）：加 `--keep-failed-gcode3mf` 重跑，解包产出
gcode.3mf 后在 plate gcode 中找边距带内的非 custom 挤出：

- 索尼克 `plate_1.gcode:2019`：`G1 X56.814 Y0.987 E0.0836`——围绕塔的
  brim 封闭轮廓（对象本体在床中央 y∈[109,163]，与此无关）；
  起始线 `G1 X185 E15 F360` 属起始 gcode（erCustom），检测不计入；
- 机械螃蟹 `plate_2.gcode:40187`：`G1 X33.548 Y272.727 E0.0628`——塔
  x 范围内的 brim 轮廓段。

**对决策选项 B（移植桌面 clamp）的关键补充：clamp 必须 brim-aware。**
索尼克是首个实测的"塔位本身合法、仅塔 brim 吃掉床边距"案例——即使塔完全
在床内，`brim_width=5` 外扩后仍可越过 0.5/1.0 边距线。这与 P-08 判例中
2026-09-09 的调研发现（clamp 不能只用塔 depth，需含 brim + cone base）互为
印证，已一并写入判例文档。

**霜花龙补充说明（报告标注"静默失败"的原因）：** 引擎实际返回
`exit_code=7 / error_code='TOOLPATH_CONFLICT'`，JSON 两字段一直有值，契约
未破。TOOLPATH_CONFLICT 按契约是 **serious_warning 级**（非 error 级），报告
工具按 `level==error` 过滤找不到失败原因，才落到"success=false 且无 error 级
issues"的兜底文案。**建议工具改读顶层 `error_code`/`exit_code` 字段**（自
2026-10-19 起恒输出，英文逗号聚合，见 `docs/requirements/exit-codes.md`）。

---

## 答SKIP：Cyber Fish —— **正确拒绝**

`PRINTER_MODEL_UNSUPPORTED: Unsupported printer model: "Bambu Lab X1 Carbon".
Only the Snapmaker U1 is supported.`——该 3MF 是 Bambu X1C 工程，超出引擎
支持机型，按设计拒绝。属预期 SKIP，无需处理。

---

## 答NO_RESULT：Faceted_Bowl_2 —— **跑批落位问题，与引擎无关**

"缺引擎 json（未切片或未落位）"发生在跑批调度层（引擎未收到任务，或产物
未落到工具扫描路径），非引擎行为。**请查该轮跑批的调度日志**；如需引擎侧
配合复跑（本地 HEAD 实测该模型可正常入队），把落位路径发我们即可。

---

## 答WARN×12：SUBSTITUTED 归因全部成立

克重偏差（2.0%–8.1%）= 密度回退精确算术（如 Matte 1.32 → Generic PLA
1.24 即 −6.06%，见 W2 二批问题四）；时间偏差（13%–69%）= 体积速度等回退
家族（如 Elegoo PLA 21 → Generic PLA 12 mm³/s 即慢 1.75×，见 W2 二批
问题一附表）。基线（桌面不替换）与引擎（云安全替换）的配置域本就不同，
这类 WARN 是替换策略的**预期效果**而非缺陷。若要降噪，工具侧可对已发生
SUBSTITUTED 的用例使用独立阈值带（或基线同样按替换后配置出值）。

---

## 请求清单（4 项）

1. **工具读 `error_code` 字段**代替按 `level==error` 过滤找失败原因（修复
   "静默失败"误标，TOOLPATH_CONFLICT 等serious_warning 级将正确呈现）；
2. **参数比对形状归一**（标量 vs 全等向量 → PASS）+ **基线对齐资源版本
   02.03.04.04**（与 W2 二批请求第 3 项合并处理）；
3. **塔族三例桌面基线产物**：SKIP 行不含桌面侧数据，如需闭环"桌面确实
   rc=0"，请提供这三个模型桌面 2.4.1 的实际切片产物（gcode 或工程截图含
   塔位），我们对勘塔几何；
4. **Faceted_Bowl_2 跑批日志**：确认该用例是否实际下发切片任务。

塔族修复方向（接受现状/移植 clamp/放宽容差）维持 2026-10-21 呈现的 A/B/C
决策选项待产品，本轮三例已作为新证据追加进判例文档（选项 B 若执行必须
brim-aware）。
