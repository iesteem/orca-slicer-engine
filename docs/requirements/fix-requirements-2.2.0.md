# 需求：2.2.0 修复项（基线 package-02.01.06）

> 来源：2026-09-16 云切片引擎审查报告 + 后续修复。**行为级验收依据**：
> `REVIEW-DIFF-020106-VS-220-2026-09-28.md`（仓库根目录）
> （package-02.01.06 vs feature-2.2.0-base @ e2e2ac9 双引擎 CLI 差分实测，
> 12/13 项已获红绿实证；REQ-12 为代码级修复，验收方式单独注明）。
> 验收夹具：`tests/fixtures/`（副本 `~/Downloads/修复验证-差分测试模型/`）。

## 一、切片正确性

### REQ-01 高低温混用检测覆盖对象级/零件级/modifier 配置
- **背景**：06 仅收集模型主体与全局键的挤出机引用，对象级 feature 键（wall/sparse_infill/solid_infill_filament）、零件级键、modifier 的 extruder 指定的高温耗材不参与混用判定，混用盘被放行（实测 5 类场景全 rc=0）。修复 cd14768。
- **需求**：切片前混用检查必须收集当前盘所有实际引用的挤出机槽位，来源至少包括：对象级 extruder 与六类 feature 键、所有 volume（含 modifier）的配置、零件级配置；仅按**实际使用**的槽位判定，不得因闲置槽装载高温耗材而拦截。
- **验收**：`temp_mode/obj_feature_*_abs_fail`、`part_feature_*_abs_fail`、`modifier_extruder_abs_fail`、`multi_plate_obj_extruder_fail` 均 rc=6 且 FILAMENT_TEMP_MIXING 带盘归因；`unused_abs_slot_ok`、`obj_feature_same_temp_ok` 仍 rc=0。

### REQ-02 空盘不再假成功，退出码与 JSON 一致
- **背景**：06 空盘静默跳过无任何 issue；单盘 `-p` 指向空盘 rc=0 却零产物；全空 rc=0 但 success=false 自相矛盾。修复 6aa4d6b（P-19）。
- **需求**：空盘必须产生 EMPTY_PLATE（warning 级）并归因到具体盘号；多盘含空盘时好盘正常产出、整体 rc=0/success=true；请求单空盘或全部为空时必须 rc=6、success=false、零产物；退出码不得晚于更具体错误码（如 slicing=4）被覆盖。
- **验收**：`slice_empty_plate_skip.3mf` 多盘 rc=0+EMPTY_PLATE(盘2)+好盘产物在；`-p 2` rc=6 零产物；`slice_all_plates_empty_fail.3mf` rc=6+两条 EMPTY_PLATE 各归因。

### REQ-03 盘号参数 `-p` 必须生效
- **背景**：06 的 C API 层 single_plate 从不赋值，`-p N` 被静默忽略，实际执行多盘全切（实测 `-p 1` 切了 4 盘）。修复 95a2246（C18）。
- **需求**：`-p N`（N≥1）必须且仅切片第 N 盘；输出产物与 JSON 统计只包含该盘。
- **验收**：`cylinder_four_plate_mixed.3mf -p 1` → JSON plates 数=1、产物仅含 plate_1.gcode（06 为 4 盘）。

### REQ-04 官方预设覆写必须整向量落地并归一槽位数
- **背景**：06 覆写按 min(dst,src) 逐元素拷贝，用户 3MF 向量短于官方槽位数时仅覆写短侧：实测 5 个 per-filament 向量卡 len=2、flush 矩阵 2×2、槽 2/3 悬空用户值（官方+用户混合配置）。修复 e2e2ac9（P-21）。
- **需求**：官方预设的向量键必须整体（wholesale）覆写；覆写前须以 filament_settings_id 槽数为权威将用户配置向量归一化；空向量须能接收官方值。
- **验收**：`u1_preset_test_truncated.3mf`（6 键截 2/4）切片后产物 project_settings.config 中全部向量 len=4 且值=官方，flush_volumes_matrix 为 4×4。

### REQ-05 auto_brim 裙边宽度回显保真
- **背景**：06 在 brim_type=auto_brim 时无条件将 brim_width 清零，gcode 头部回显 `brim_width = 0`，与用户配置（5）不符。修复 f291619 + b79415d（P-11/P-20）。
- **需求**：auto_brim 的宽度清零只允许发生在切片副本上；输出 gcode 头部与产物配置的 brim_width 必须回显用户原值；回显改写须有流状态校验，失败时保留原文件、降级 warning，不得放大为整单报废；匹配须锚定 CONFIG_BLOCK 区间且校验旧值，防外部注入劫持。
- **验收**：`high_flow_whitelist.3mf` 产物 gcode 头 `; brim_width = 5`（06 为 0）；切片运动行与 06 逐行一致。

### REQ-06 喷嘴流量类型（nozzle_volume_type）跨预设覆写保留
- **背景**：官方打印机预设无此键，wholesale 覆写将用户逐槽硬件状态重置为全 standard（实测 06 产物 `["standard"×4]`）。修复 3640a49。
- **需求**：官方覆写前快照用户 nozzle_volume_type，覆写后按官方槽位数嫁接回写；该键仅影响回显/写回保真，不得改变切片行为。
- **验收**：槽 0=high_flow 的 `high_flow_whitelist.3mf` 产物为 `["high_flow","standard","standard","standard"]`（06 为全 standard）。

## 二、安全与健壮性

### REQ-07 CLI→引擎 params JSON 构造安全
- **背景**：06 用 2048 缓冲 + `pos += snprintf`（返回值误用）：cancel_file/log_path/json_output_path 含引号即注入破坏 JSON（实测 rc=1 parse error）；路径超长时截断产生非法 JSON。**注意：input 文件名为 `slic3r_slice()` 独立参数不进 JSON，非注入面（审查报告 M6 原表述据此修正）。** 修复 95a2246（N1）。
- **需求**：所有进入 params JSON 的字符串必须经 JSON 转义（append_json_str）；缓冲追加必须 bounds-checked、pos 越界即停止；缓冲容量（8192）须覆盖现实路径长度。
- **验收**：`--cancel-file '/tmp/c"x'` → rc=0 正常切片（06 rc=1）；2186 字符 `-j` 路径 → rc=0 且 stats.json 正常产出（06 rc=1 截断）。

### REQ-08 超短 PNG 缩略图在签名比较前拒绝
- **背景**：06 仅限 4MB 上限，3 字节文件直达 magic 比较，读取 8 字节签名发生堆越界读（无 ASAN 时静默，日志 `bad PNG magic`）。修复 95a2246（N2）。
- **需求**：解码前文件长度小于 PNG 签名长度（8 字节）必须拒绝，不得进入 magic 比较；缩略图失败不阻断切片（尽力而为语义不变）。
- **验收**：`thumbnail_short_png.3mf`（3 字节）rc=0 且日志 `skip plate 0, bad size 3`（06 为 `bad PNG magic` 路径）；ASAN 构建下无 heap-buffer-overflow 报告。

### REQ-09 预设装载失败硬门控（含 skip 场景）
- **背景**：06 在 `--skip-preset-substitution` 下系统预设加载失败仍继续，后续 is_bbl_vendor/get_current_vendor_type 解引用空配置 → SIGSEGV（实测 rc=139 core dump，所有输入必崩）。修复 95a2246（N3，判例 P-09）。
- **需求**：load_system_presets 成功与否必须无条件门控后续流程；skip 开关只跳过替换逻辑，不得跳过预设可用性校验；失败路径必须输出结构化 JSON（PRESETS_MISSING）与 rc=6。
- **验收**：`--skip-preset-substitution -r <空目录>` → rc=6 + PRESETS_MISSING + JSON 可解析（06 rc=139 无输出）。
- **备注（2026-09-29）**：`--skip-preset-substitution` 参数已整体移除；硬门控需求仍然有效。验收命令中的 flag 现被静默忽略（门控行为等价，回归测试 tests/test_c_api.cpp P-09 用例继续覆盖）。


### REQ-10 CLI/C API 边界参数校验
- **背景**：06 用 atoi 解析数值（`-t abc`→0=静默无超时）、未知 format 静默按默认执行、负 plate_id 照收（三项实测均 rc=0 静默继续）。修复 95a2246。
- **需求**：数值参数（plate_id/timeout/max_size）必须 ≥0 且为合法整数，否则拒绝退出 rc=1；format 必须白名单（gcode | gcode.3mf）；校验在 API 边界与 CLI 双层生效。
- **验收**：`-f xyz`、`-t abc`、`-p -1` 三项均 rc=1 且带明确错误信息（06 全部 rc=0 静默执行）。

### REQ-11 输出路径搬迁可观测
- **背景**：06 输出路径被占用时静默换写时间戳路径，编排方按原路径取产物会取空。修复 af80f0b。
- **需求**：路径搬迁行为保留（不失败），但必须发 OUTPUT_PATH_RELOCATED（warning）issue，且 JSON 的 output_file 反映真实路径。
- **验收**：预占位 `out.gcode.3mf` 后切片 → rc=0 + JSON 含 OUTPUT_PATH_RELOCATED + output_file 指向时间戳文件（06 零告警）。

### REQ-12 日志 sink 生命周期管理
- **背景**：06 重复添加文件/控制台 sink，同进程多次任务（长驻 DLL 模式）会日志重复与句柄残留。修复 95a2246。**☆ 代码级修复，CLI 单任务不可见。**
- **需求**：添加文件 sink 前必须移除既有文件 sink；控制台 sink 与公共属性去重。
- **验收**：需 C API 同进程连续两次切片任务，断言日志无重复行、无句柄泄漏（CLI 单次运行无法暴露，见报告第二节）。

## 三、交付与打包

### REQ-13 生产 bundle 打包出厂自检（filament_vendor 继承链）
- **背景**：bundle 打包时从 fork 实时拷贝、无锁定；坏 bundle 进入生产会使所有请求在预设装载阶段整单致命（P-09 无降级）。06 打包无任何校验。修复 e33d9f4（C5 闭环）。
- **需求**：打包脚本必须在产出前对**打进产物的 bundle** 自检：所有 instantiated 耗材预设沿 inherits 链合并后必须解析出 filament_vendor；链不得悬空、成环；JSON 必须可解析；空 bundle 拒绝（防空目录伪绿）。任一失败中止打包。
- **验收**：健康 bundle 通过（312 预设/222 实例化/0 悬空/0 缺）；删除 `Generic ASA @U1 base` → rc=1 报 4 悬空+4 缺 vendor；坏 JSON → rc=1；空目录 → rc=1（红绿矩阵实测见报告第三节；构造红测须删 instantiated 预设的直接父级）。

## 边界（不计入 2.2.0 修复）

- **S1 空壳内嵌预设**：06 与 2.2.0 行为逐项相同（均 rc=6，由 06 线既有的 prime tower 升级拦截），非 2.2.0 增量。
- **C6 脏 printer_settings_id**（`MyToolChanger 0.4 nozzle - Copy`）：两版产物均现，未修（根因在 bundle/fork JSON 本身）。
- b5c5bde（flow-variant 向量）提交早于 06 包构建 3 小时，视为 06 已含。
- 其余未修项（B1/B2/B3/B4/B6、S2-S6、E4/E5 等）见审查报告与 docs/precedents/。
