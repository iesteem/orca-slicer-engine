# P-33: 温度混用检测不收集 layer_config_ranges——高度范围换高低温槽被放行（已修复）

> **状态：** 已修复（2026-10-22，W2 二批问题三路径 B；用户拍板"现在修"）。
> 修复：`check_filament_temp_mixing` 收集器补 ranges 来源——helper 参数
> `const ModelConfigObject&` → `const ModelConfig&`（公共继承上转，零重复
> 逻辑），对象循环内逐 range 收集。测试：test_temp_mode 新增 2 用例，
> 18/18 绿；CLI 实测修复前 exit 0（gcode T0+T1 双工具真混用）→ 修复后
> exit 6 `FILAMENT_TEMP_MIXING`。
> **分歧声明：引擎自此比桌面更严**——桌面 `Plater::check_filament_temp_mixing`
> （Plater.cpp:22343）收集器逐行同构、同样不读 ranges（其 issue#5 挂起族），
> 修复后引擎会拦下桌面放行的 range 高低温混用文件。云端拒切面扩大是有意
> 之为：V05 规格（测试文件名"预期切片失败"）本就要求拦截，且实际打印输出
> 确实在 range 边界发生高低温耗材切换（有喷嘴损伤风险）。
> 同批**路径 A**（全局-支撑筏层界面键被工艺替换清零后检测自然不触发）
> **不属本案**：根因是 P-30 wholesale 替换语义，是否豁免待产品决策。

---

## TL;DR

高度范围（layer_config_ranges）可以把对象某段 z 区间的
wall/sparse/solid_infill_filament 切到另一槽。引擎与桌面的温度混用
检测器（`check_filament_temp_mixing`）的槽位收集器都**只**读对象配置、
体素配置、mmu 涂色和全局键，**不读 ranges**——range 引入高低温槽组合
时，实际 gcode 已双工具切换，检测却放行 exit 0。测试队 W2 二批
（2026-10-22 送修单问题三）两条路径中的"自定义层高，高低温混用"即此。
修复后引擎比桌面多拦一类文件（有意分歧），上游建议随 Orca issue#5
族一并对齐。

## 1. 现象与复现

**原始报告：** W2 二批问题三（`docs/process/engine-reply-w2-round2-2026-10-22.md`）：
020200 专项 16 例高低温混用中 2 例引擎 exit 0 放行，其中"自定义层高"路径
即本缺口（另 14 例对象/全局/涂色路径均正常报错，证明检测器本体工作正常）。

**本地复现（决定性）：** 捐赠者 fixture `tests/fixtures/temp_mode/unused_abs_slot_ok.3mf`
（2 槽：1=Generic PLA 低温 / 2=Generic ABS 高温，ABS 槽闲置），注入
`Metadata/layer_config_ranges.xml`（`[Content_Types].xml` 无需改动）：

```xml
<objects>
 <object id="1">            <!-- 1-based 导入序号，≠ model_settings 的对象 id -->
  <range min_z="0" max_z="5">
   <option opt_key="extruder">0</option>
   <option opt_key="layer_height">0.2</option>
   <option opt_key="solid_infill_filament">2</option>
   <option opt_key="sparse_infill_filament">2</option>
   <option opt_key="wall_filament">2</option>
  </range>
 </object>
</objects>
```

修复前实测：exit 0，产物 gcode **T0+T1 双工具**（0-5mm 段 ABS、其余 PLA，
真混用被放行）；修复后：exit 6 + `FILAMENT_TEMP_MIXING`。

**fixture 构造坑（W2 取证阶段实证，复用必读）：**
- ranges 的 object id 是 **1-based 导入序号**（见判例 layer-ranges 经验），
  不是 model_settings.config 里的对象 id（捐赠者对象 id=12，ranges 写 1）。
- 部分区间（如 0-5mm）比全包区间（0-999）更贴真实场景且 gcode 出双工具，
  断言更有力；全包时整件单工具，"引用即混用"但输出无切换。
- 想让各槽挂**不同官方预设**（构造高低温差），改 `filament_settings_id`
  无效——必须按槽改 `filament_ids`（GFSB99=Generic ABS / GFSL991=Generic
  PLA），否则整槽被 tray id 压平成单一预设，温差构造失败。

## 2. 根因

- 引擎收集器（SliceEngine.cpp `check_filament_temp_mixing`）来源仅四类：
  obj->config、每 vol->config（含 modifier）、vol->get_extruders()（mmu
  涂色）、全局 always/conditional 键。`obj->layer_config_ranges` 无任何
  来源覆盖。
- 桌面 `Plater::check_filament_temp_mixing`（Plater.cpp:22343）收集器
  逐行同构，同样不读 ranges → **双端同漏**，即测试队引述的 2.2.0 报告
  issue#5（"高度范围修改器创建后立即修改耗材，混用检测不立即生效"）族。
- 邻域先例：fork `5cac51bbe` 已为 `Print::object_extruders` 补过**同构
  缺口**（layer_config_ranges 的 wall/sparse/solid_infill_filament，判例
  wipe-tower-layer-config-ranges.md）——那是擦塔启用误判，本案是同一键族
  在混用检测器上的对应缺口。

## 3. 修复（2026-10-22）

1. `collect_filament_slots_from_model_config` 参数放宽为 `const ModelConfig&`：
   `ModelConfig` 基类具备 `has/opt_int/extruder()` 全套 const API，
   `ModelConfigObject` 公共继承自它，原三个调用点（对象/体素）上转绑定不变。
2. 对象循环内新增：

```cpp
for (const auto& range_entry : obj->layer_config_ranges)
    collect_filament_slots_from_model_config(range_entry.second, num_filaments, used_slots);
```

   同一收集器即同一语义：`[1, num_filaments]` 界外忽略、`0 = inherit` 哨兵
   忽略、六 feature 键 + extruder 全覆盖。与收集源清单注释（ii+ 条目）同步。

## 4. 影响面

- **行为变化：** range 引用高低温槽组合（收集后 has_high && has_low）的
  3MF：exit 0 → exit 6（error 级 FILAMENT_TEMP_MIXING，盘级拒切）。
- **零影响面：** 单温库 ranges（全 PLA / 全 ABS 等，槽全同类）——收集了
  也不触发 high/low 组合；无 ranges 文件完全不进入新代码路径。
- **桌面 parity：** 引擎更严（桌面放行、引擎拒切）。上游修复 issue#5 族后
  双端自然对齐；期间云端多拦的文件符合 V05 规格预期。

## 5. 测试

- `tests/test_temp_mode.cpp` 新增 2 用例：
  `Layer-range ABS band + PLA body must fail temp mix`（必拦）、
  `Layer-range same-temperature band must not fail temp mix`（不误伤）。
- fixtures 入库：`range_feature_abs_fail.3mf` / `range_same_temp_ok.3mf`
  （构造法见 §1）。
- 专项：tempmode 18 用例 53 断言全绿；CLI 级：RC=6、JSON error codes=
  [FILAMENT_TEMP_MIXING]。
- 全量回归（2026-10-22）：225 用例（基线 223 + 新增 2）；6 例红为
  **并行会话假红**（accuracy×1 / filament_count×1 / filament_rollback×2 /
  path_conflict×2——另一会话同时段在跑 87fa 验证切片；accuracy 系 fixture
  无 ranges，本修复不可能触及），6 例逐一单跑全绿，符合
  "套跑须独占机器"判例；其余 219 例直接绿。

## 7. 回执复核（2026-10-22 晚，测试组送检原件）

测试组回执送来两条原始 3mf。静态 diff：**两条配置 100% 相同**（唯一差异
`identify_id`），`support_interface_filament="1"`（PLA 低温槽，并非其
README 所述"挂高温槽"构造），高温 ABS 仅由 layer_config_ranges 引用
（range 0-2：extruder=3 + wall_filament=3）——**两条均为 ranges（B 路径）
构造**。实测（修复版二进制）：**两条均 RC=6 + FILAMENT_TEMP_MIXING**，
与其文件名预期"切片失败"及 V05 规格一致，全部由本案修复覆盖。

"全局支撑界面键挂高温槽"的真实构造**未落到送检文件**（同文件
sparse/solid_infill_filament=2 均已持久化，support 键却为 1）——待测试组
检查导出流程重新出件；该路径机制已由引擎侧重构实验（V1/V4）单独实证为
**替换清键**（P-30 wholesale 家族），届时按 P-30 豁免决策处理。

## 6. 关联

- P-30（wholesale 替换豁免）：路径 A（support/wipe_tower 耗材键被清）待
  产品决策后同法处理。
- 上游 Orca issue#5 族（桌面收集器同漏）。
- W2 二批答复：`docs/process/engine-reply-w2-round2-2026-10-22.md`。
- 同构先例：`docs/precedents/wipe-tower-layer-config-ranges.md`（5cac51bbe）。
