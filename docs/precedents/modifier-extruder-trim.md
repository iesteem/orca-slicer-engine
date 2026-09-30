# P-04: 多色被误判单色（modifier extruder 漏收 + trim 误裁）

**状态：** trim 已物理移除（commit 02a4135，feature-02.01.04）
**Fixture：** filement_count/ modifier 系列；"扭扭眼球玩具.3mf"（外部）

## 两个连环 bug（均已终结于"删除 trim"）

1. **modifier extruder 漏收**：`used_extruders` 收集循环的
   `if (!vol->is_model_part()) continue;` 丢弃 modifier volume 的 extruder。
   权威对照：libslic3r `Print::object_extruders()` 不过滤 modifier，
   `ModelVolume::get_extruders()` 只排除 INVALID/NEGATIVE/SUPPORT_BLOCKER/
   SUPPORT_ENFORCER，**包含 PARAMETER_MODIFIER**。
2. **trim 漏查第三源**：trim 决策不查 `layer_config_ranges`（libslic3r
   object_extruders 的第三源），把高度范围换色模型误判单色裁成单槽。

## 终局方案（路径 B：删 trim）

把多色判定完全交给 libslic3r 的 `object_extruders()`/`ToolOrdering`。
6 模型三方实测（带 trim / 环境变量禁用 / 物理删除）：

- 删 trim 后**零崩溃**——libslic3r 自己能判定擦除塔，trim 不是必须
- 保留真实槽位语义（trim 曾把 slot2 remap 到 idx0，stats 报错槽）
- 注意：删 trim 修好 stats 双色 ≠ 擦除塔正确（见 P-03）

## 教训

引擎侧任何"预判定"（trim、计数、规整）都是与 libslic3r 内部逻辑竞争的
第二事实源，一旦上游多考虑一个来源就分叉。**能交给 libslic3r 判定的，
引擎不要自己再判一遍。**
