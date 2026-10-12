# P-25 输出 3MF 盘元数据 `support_used` 换源 gcode 实际层

> 2026-10-10。人类批准推翻同日 D1 parity 决策（"输出 3MF 侧保持配置层"）。
> 与 P-05/P-06 同类：契约决策，非缺陷修复——但动机包含一个真实语义风险
> （配置层对云消费方主动撒谎，见"根因"）。

## 现象

`PlateSliceResult` 曾同时携带两个支撑标志：

- `support_used`（配置层，`Print::is_support_used()`）→ 唯一消费方：输出
  gcode.3mf 盘元数据 `Metadata/slice_info.config` 的 `key="support_used"`
  （SliceEngine.cpp relay → `PlateData::is_support_used` →
  bbs_3mf.cpp:7880 无条件序列化）。桌面 PartPlate.cpp:5476 写的同为配置层。
- `support_generated`（gcode 实际层，`support_volumes_per_extruder` 求和>0）
  → 唯一消费方：JSON `support_free = !support_generated`（两级）。

云消费方只关心"3MF 切片结果中实际有没有生成支撑"，不关心工艺设置是否
开启支撑——双成员是为桌面 parity 保留的。

## 根因（为何可以推翻 parity）

全 fork 树取证（2026-10-10，grep 全树）：`is_support_used` 除
定义/写出/读回/回显日志外**零真实消费方**——纯 write→parse→rewrite
roundtrip，GUI 无图标、无逻辑、无上传路径读它。parity 的实际收益只剩
"与桌面产物元数据逐字节一致"，树内不保护任何可观测行为。

而消费方若解析该键判断实际支撑，配置层在混合 case（enable_support 开 +
自支撑几何，如 `geom_above_bed_support_on.3mf`）**主动给错值**：说生成了、
实际零挤出。用户确认无"云 vs 桌面产物元数据比对"流程后批准换源。

## 修复

- 删 `PlateSliceResult::support_used` 成员及其赋值；
- relay 换喂 `pd->is_support_used = result.support_generated`；
- JSON `support_free` 与 3MF `support_used` 从此互补同源（同一事实两面）。

## 验证证据

`[capi][support]` 两判别用例各锚一条输出 3MF 断言（解包
`Metadata/slice_info.config`）：

- `geom_above_bed_support_on.3mf` → `key="support_used" value="false"`
  （换源前为 true——该断言在旧代码下必红，锚住翻转）；
- `geom_overhang_support_generated.3mf` → `value="true"`（真生成侧）。

## 已证伪/已否决的替代方案

- **删掉 3MF 里的字段**：不可行——上游 bbs_3mf.cpp:7880 无条件序列化
  `PlateData::is_support_used`，引擎不 patch fork 时该键必然在场，唯一
  自由度是喂值。
- **反向合并（JSON 回配置层）**：即 f67f630 修掉的旧语义，配置开+自支撑
  误报"需要支撑"，禁止。
