# P-21: 预设替换的向量短侧截断（审查报告 C3，替换不完整+残留旧值）

**状态：** 已修复（2026-09-20）。
源头：2026-09-16 云切片审查报告第四节 C3 → 2026-09-18 66-agent 对抗验证标记
still_present（两处代码与基线逐字相同、全 src 无 set_num_filaments 规整、
测试对短 dst 静默通过）→ 2026-09-20 修复。

## 现象（修复前）

三个方向的数据损坏，全部静默：

1. **1.4a/1.4c 整体覆盖按 min(dst,src) 逐元素截断**（`overwrite_all_keys_from_impl`
   向量分支）：src 短则 dst 尾部槽位残留旧值；dst 短则官方值被丢弃。
   空 dst 向量（如 3MF 未携带 `compatible_printers`）永远收不到官方值。
2. **1.4b 逐槽替换整槽静默跳过**（`overwriteExtruderFrom` 中
   `dst_vec->size() <= dst_idx → continue`）：3MF 声明 4 槽但某 per-filament
   向量只有 2 条目时，槽 2/3 的该键**保留原始 3MF 用户值**——替换的目的
   （用官方值覆盖不可信用户值）在这些槽位落空，用户控制的参数直接流入切片。
3. **全程无长度规整**：桌面版 `Preset::normalize` 在每次预设加载/应用后对齐
   per-filament 向量长度，引擎完全没有这一步。

红测试实证（`u1_preset_test_truncated.3mf`：6 个 per-filament 向量截 2/4、
`filament_settings_id` 保 4 槽）：修复前 21 断言失败——6 键长度仍 2、
flush 矩阵 2×2、槽 2/3 值为空或残留。

## 根因链（三缺陷 + 一个上游发现）

- **D1** `overwrite_all_keys_from_impl`（SliceEngine.cpp）向量分支
  `for (i < dst.size() && i < src.size()) set_at(i,i)`——7802c66 初版实现，
  无回归驱动的理由；桌面 `ConfigBase::apply` 是整选项替换，从不逐元素 min 拷贝。
- **D2** `PresetRollback::overwriteExtruderFrom` 的 `continue` 守卫吞掉短向量
  的整槽替换（common-error-handling C 级违例：静默吞错误条件）。
- **D3** 引擎无 `set_num_filaments` 式长度规整；下游（flush 矩阵、
  setup_extruder_params、temp mixing）各自用不同键的 size() 推槽数，互相矛盾。
- **上游发现（重要）**：fork 的 `DynamicPrintConfig::set_num_filaments()` 本身
  是**彻底的 no-op**——`PrintConfigDef` 构造函数没有调用
  `init_filament_option_keys()`，且该函数**全树零调用者**，
  `print_config_def.filament_option_keys()` 恒为空，set_num_filaments 的循环
  迭代空表。桌面端同样中招但不自知：`Preset::normalize` 对它的调用无效，
  核心键（含 `filament_colour`）的长度实际由 GUI 层
  `PresetBundle::set_num_filaments`（另一个同名函数）维护。引擎无 GUI 层，
  所以必须在引擎侧自建。实证：临时日志显示 `set_num_filaments(4)` 前后
  `filament_colour` 长度均为 2；而 `Preset::filament_options()`（静态数据表，
  不依赖 init 调用）驱动的 grow-only 通道工作正常。

## 已证伪的推测（勿重走弯路）

- **"直接调 `Preset::normalize` 即可"**——错。它从 `filament_diameter.size()`
  取 n，而截断 3MF 恰恰污染该键（本例截到 2），会把整个项目错切成 2 槽。
  n 必须取 `filament_settings_id.size()`（1.4b 循环的既有权威，桌面 Plater
  同源）。
- **"修 D2 就修好了温度"**——不成立。`nozzle_temperature` 等温度键是
  flow-variant（分段存储），`overwriteExtruderFrom` 刻意跳过它们（b5c5bde），
  从不逐槽覆写；对它们的修复是**长度**正确（grow-only 到段计数），值仍来自
  3MF 透传。
- **"b5c5bde 已修此问题"**——它修的是 flow-variant 键的段对齐**值污染**
  （flat set_at 塌缩 25→15），与长度语义无关。
- **"set_at 空向量自动增长"**——`set_at` 在 dst 非空时才自动增长（用
  front() 填充中间槽位）；**空向量上是 UB**（release 下 front() 于空 vector）。
  D2 修复必须保留空向量跳过。

## 修复方案（桌面 parity：整体覆盖 + 槽数权威规整）

1. **`overwrite_all_keys_from_impl` 向量分支改整体拷贝**：与 scalar 分支对称的
   类型守卫 + `dst_opt->set(src)`，等价桌面 `apply()` 语义。空数组也逐字拷入
   （`compatible_printers` 等键因此能收到官方值）。
2. **`normalize_filament_vectors_to_slot_count(config)`**（SliceEngine.cpp
   匿名 namespace，新增）：
   - n = `filament_settings_id.size()`（缺失回退 max(filament_diameter,
     filament_colour) 长度）；
   - **引擎侧核心键精确 resize**（grows AND shrinks，23 键镜像
     `init_filament_option_keys` 键表、同款 flow-variant 跳过）——因上游
     set_num_filaments 是 no-op，见根因链第 4 条；
   - grow-only 遍历 `Preset::filament_options()`：缺失键从
     `FullPrintConfig::defaults()` clone 回填，短向量补到 n（flow-variant 键
     补到段计数：`filament_flow_step_size.size()==n` 时为 sum(max(1,step))，
     n==1 且有声明列表时为其长度，否则 n）——移植自 `Preset::normalize`；
   - `filament_settings_id` grow-only 到 n；`filament_ids`（AMS 数组）刻意
     不动；`nozzle_diameter`/`set_num_extruders` 分支刻意不移植（挤出机数
     归官方机预设所有）。
3. **两个调用点**（幂等）：`apply_preset_substitution` 内 `strip_user_content()`
   之后（1.4b 前保证所有槽位可写）；`apply_printer_official_preset` 内
   整体覆盖之后、nozzle_volume_type 嫁接之前（官方机预设携带 size-4 的
   per-filament 向量，wholesale 后必须回到项目槽数）。
4. **`overwriteExtruderFrom`（D2）**：空向量保留 continue（UB 防御）；
   非空但短改为 warning 日志 + `set_at` 自动增长——槽位必须被替换，
   不再静默跳过。

## 修改内容

| 文件 | 变更 |
|---|---|
| `src/SliceEngine.cpp` | impl 向量分支整体拷贝；新增 normalize helper；两个调用点 |
| `src/PresetRollback.cpp` | D2：空跳/短警告+增长，函数头注释更新 |
| `tests/test_preset_substitution.cpp` | :213 一刀切 continue 改向量类型感知断言（槽覆盖 FAIL_CHECK）；删 compatible_printers / small_area_infill_flow_compensation_model 两个 overwrite-semantics 豁免；新增 "Truncated per-filament vectors normalized to slot count" 用例 |
| `tests/fixtures/u1_preset_test_truncated.3mf` | u1_preset_test 派生：6 键截 2/4、settings_id 保 4（MANIFEST 已登记） |

## 验证证据

- **红**（修复前，2026-09-20 16:13 实测）：新用例 21 断言失败（6 键 len=2、
  flush 矩阵 != 16、槽 2/3 空/残留）；主用例去豁免后 2 键 mismatch
  （compatible_printers=""、small_area=""）。
- **绿**（修复后）：`[preset]` 5 用例 76 断言全绿；全量
  `engine-integration` 116 用例 658 断言全绿（修复前基线 115 用例 620 断言）；
  `orca-slice-engine` 目标编译通过。
- **上游 no-op 实证**：C3DBG 临时日志（已移除）显示 set_num_filaments(4)
  前后 filament_colour len=2 不变；改引擎侧 resize 后立即归 4。

## 遗留问题（识别、未处理）

1. **上游 fork 缺陷未上报/未修**：`PrintConfigDef` 构造缺
   `init_filament_option_keys()` 调用 → 全 fork 的 set_num_filaments 死代码。
   引擎侧已自建键表绕开；若上游修复（构造补调用），引擎 helper 的精确
   resize 保持正确（语义相同，幂等），可再评估收敛。
2. `set_num_extruders` 的 `init_extruder_option_keys()` 构造有调用、不受影响。
3. 收缩语义为**有意超越桌面**的行为：桌面实际从不收缩核心键（其
   set_num_filaments 是 no-op、GUI 层才管），引擎现在会把超长向量（如 8 色
   `filament_multi_colors` 在 4 槽项目）截到 n。与桌面 Plater 的 GUI 层行为
   一致（PresetBundle::set_num_filaments 同样 resize 到 n）。
4. `.json`（仓库根）来历不明的未跟踪杂散文件，未处理。
