# `.gcode.3mf` 边写边释放 G-code 内存 — 可行性分析

> 基于「云切片引擎 v02.01.10 批量验证报告」中 P1 建议的可行性调研

## 一、问题回顾

### 1.1 OOM 现象

| # | 模型 | 文件大小 | 失败阶段 | 退出码 |
|---|------|---------|---------|--------|
| 9 | 醒狮眼镜仔_木公 | 19 MB | G-code 导出 | 137 (OOMKilled) |
| 10 | 醒狮眼镜仔_木公（6） | 107 MB | G-code 导出 | 137 (OOMKilled) |
| 11 | 醒狮眼镜仔_木公（12） | 214 MB | G-code 导出 | 137 (OOMKilled) |

Pod 累计被 OOMKilled 6 次，全部发生在 G-code 导出阶段。

### 1.2 内存增长曲线（19MB 版本）

```
VmRSS
15.7G ┤                                                                      ╱
      │                                                                  ╱╱╱
14.0G ┤                                                              ╱╱╱
12.0G ┤                                                          ╱╱╱
10.0G ┤                                                      ╱╱╱
 8.0G ┤                                               ╱╱╱╱
 6.0G ┤                     ▁▁▁▁▁╱╱╱╱                                    Support Gen
 4.0G ┤          ▁▁▁▁▁╱╱╱
 2.0G ┤ ▁▁▁▁╱╱╱╱              Slicing 稳定期
    0 ┼───────────┬───────────┬───────────┬───────────┬───────────┬───────────→
                Startup     Slicing    Support Gen   G-code Export (~120s, ~60 MB/s)
```

- G-code 导出阶段以 **~60 MB/s** 线性增长
- 峰值 VmRSS **15.0 GiB**（含 Java Worker 基线共 ~23 GiB，超出 Pod 限制 16 GiB）
- 引擎把全部 plate 的 gcode 数据一直保留在内存中直到写入完成

## 二、根因分析

### 2.1 数据结构

`GCodeProcessorResult`（`GCodeProcessor.hpp:134-279`）包含两个大向量：

```cpp
// GCodeProcessor.hpp:196-198
std::vector<MoveVertex> moves;     // 每个 MoveVertex ≈ 150 字节
std::vector<size_t> lines_ends;    // G-code 文件中每个换行符的字节偏移量
```

每个 `MoveVertex`（`:152-185`）包含：位置、挤出量、进给率、宽度、高度、温度、风扇速度、弧线插值点等 ~20 个字段。

### 2.2 内存增长链路

```
GCode::do_export()                           // GCode.cpp:1749
  └─ GCodeOutputStream::write()              // GCode.cpp:7189
       ├─ fwrite(gcode_line)                 // 文本已流式写入磁盘 ✅
       └─ m_processor.process_buffer()       // GCodeProcessor.cpp:1302
            └─ process_gcode_line()
                 └─ store_move_vertex()      // GCodeProcessor.cpp:4773
                      └─ m_result.moves.push_back({...})   // ← 内存线性增长
  └─ *result = std::move(m_processor.extract_result())

SliceEngine::export_gcode()                  // SliceEngine.cpp:1857
  └─ print.export_gcode(..., &result.gcode_result, ...)
  └─ 返回后 result.gcode_result.moves 已包含全部移动顶点

SliceEngine::process_plate()                 // SliceEngine.cpp:1410
  └─ m_plate_results[plate_id] = slice_result   // 所有 plate 结果一直保留
```

### 2.3 关键发现：moves 的使用点

遍历整个引擎流水线，`moves` 和 `lines_ends` 仅在以下位置被访问：

| 阶段 | 位置 | 用途 | 依赖 moves? |
|------|------|------|------------|
| postprocessing | `SliceEngine.cpp:1973-1986` | 遍历 moves 找最大 Z 做高度越界检查 | **是 ← 唯一使用点** |
| package_output | `SliceEngine.cpp:2100-2116` | 读 print_statistics / filament info | 否 |
| build_statistics | `SliceEngine.cpp:2243-2350` | 读 print_statistics / filament info | 否 |

**结论**：`moves` 向量仅在 `run_postprocessing()` 做 max-Z 检查时需要。其余所有后续阶段只需要 `print_statistics`、`filament_diameters` 等轻量元数据。

### 2.4 两层问题

| 层 | 描述 | 影响 |
|----|------|------|
| **A** | 单 plate 内 moves 增长过大，OOM 发生在 `do_export()` 执行期间 | 报告中的 3 个失败 |
| **B** | 多 plate 的 moves 全部保留在 `m_plate_results` 中，内存不释放 | 多 plate 场景 |

## 三、方案 1：释放已完成 plate 的 moves（解决层 B）

### 3.1 改动方案

1. 在 `GCodeProcessorResult` 中新增 `float max_z_height` 字段，在 `store_move_vertex()` 中增量更新
2. `run_postprocessing()` 改用 `result.gcode_result.max_z_height` 替代遍历 moves
3. 在 `run_postprocessing()` 末尾 clear moves 和 lines_ends：

```cpp
// 在 run_postprocessing() 末尾：
result.gcode_result.moves.clear();
result.gcode_result.moves.shrink_to_fit();
result.gcode_result.lines_ends.clear();
result.gcode_result.lines_ends.shrink_to_fit();
```

### 3.2 评估

| 维度 | 评估 |
|------|------|
| 解决层 A | ❌ 不解决（OOM 发生在 do_export() 内部） |
| 解决层 B | ✅ 解决 |
| 改动量 | ~20 行 |
| 风险 | 低 |
| 涉及文件 | `GCodeProcessor.hpp`、`GCodeProcessor.cpp`、`SliceEngine.cpp` |

---

## 四、方案 2：GCodeProcessor Cloud Mode — 跳过 moves 收集（解决层 A）

### 4.1 核心思路

在 `GCodeProcessor` 中新增 `m_cloud_mode` 标志，启用后不再将每个移动顶点存入 `m_result.moves` 向量，而是只追踪：
- 最后一个位置（替代 `moves.back().position`）
- 移动计数（替代 `moves.size()`）
- 最大 Z 高度（增量计算，替代 `run_postprocessing()` 中遍历全部 moves）
- 其他轻量统计数据

**预期效果**：对于生成 5000 万次移动的模型，moves 内存从 ~7.5 GB 降至 ~100 字节。

### 4.2 GCodeProcessor 对 `m_result.moves` 的依赖全景

完整遍历 `GCodeProcessor.cpp` 和 `.hpp`（共 5112 行），所有 `m_result.moves` 的访问分为三类：

#### 类别 A：仅需最后一个或最近几个元素 — 可用轻量替代

| 文件:行号 | 表达式 | 用途 |
|-----------|--------|------|
| `hpp:594` | `moves.size() - 1` | OptionsZCorrector::set() — 记录当前 move 索引 |
| `hpp:602` | `moves.back().position` | OptionsZCorrector::update() — 获取最后位置 |
| `hpp:604` | `moves.emplace_back(...)` | OptionsZCorrector::update() — 复制一份 move 到末尾 |
| `hpp:607` | `moves.erase(begin + *m_move_id)` | OptionsZCorrector::update() — 删除中间的旧 move |
| `cpp:2083` | `moves.empty()` | process_tags — 安全检查 |
| `cpp:2087` | `moves.size() - 1 - m_seams_count` | process_tags — 计算最后一个非 seam move 的索引 |
| `cpp:2964` | `moves.back().position` | process_G1 — seam 检测：获取上一个位置 |
| `cpp:2983` | `moves.back().position` | process_G1 — seam 检测：获取上一个位置 |
| `cpp:2998` | `moves.back().position` | process_G1 — seam 检测：设置起始顶点 |
| `cpp:3011` | `moves.empty()` | process_G1 — 安全检查 |
| `cpp:3012` | `moves.size() - 1 - m_seams_count` | process_G1 — spiral_vase_layers 结束索引 |
| `cpp:3399` | `moves.back().position` | process_G2_G3 — seam 检测（同 process_G1） |
| `cpp:3416` | `moves.back().position` | process_G2_G3 — seam 检测 |
| `cpp:3431` | `moves.back().position` | process_G2_G3 — seam 检测：设置起始顶点 |
| `cpp:3446` | `moves.empty()` | process_G2_G3 — 安全检查 |
| `cpp:3447` | `moves.size() - 1 - m_seams_count` | process_G2_G3 — spiral_vase_layers 结束索引 |
| `cpp:4790` | `moves.push_back({...})` | store_move_vertex — **主要的累积点** |
| `cpp:4806` | `moves.size()` | store_move_vertex — 用作当前 move 的 time 占位符 |

#### 类别 B：遍历全部 moves — Cloud 模式不需要

| 文件:行号 | 代码 | 用途 | Cloud 需要? |
|-----------|------|------|------------|
| `cpp:1313-1318` | `for (move : moves)` 设置 Wipe width/height | 为 GUI 渲染修正 Wipe 移动的线宽 | **否** |
| `cpp:1338-1346` | `for (i : moves)` 设置 layer_duration | 将每个 move 的临时 layer_id 替换为实际层耗时 | **否** |
| `cpp:4374-4383` | `for (move : moves)` 同步 gcode_id | M73 行插入后重新映射行号，供 GCodeViewer 顺序渲染 | **否** |

#### 类别 C：初始化

| 文件:行号 | 代码 | 用途 |
|-----------|------|------|
| `cpp:1271` | `moves.emplace_back(MoveVertex())` | `process_file()` 中插入初始哑 move |
| `cpp:1299` | `moves.emplace_back(MoveVertex())` | `initialize()` 中插入初始哑 move |

### 4.3 关键依赖深度分析

#### 4.3.1 Seam 检测（类别 A 核心依赖）

Seam 检测器在 `process_G1` (:2960-2999) 和 `process_G2_G3` (:3396-3435) 中使用 `m_result.moves.back().position` 获取**上一个**移动的终点位置，用于判断当前外壁移动是否与之前的移动闭合形成接缝。

**实际需要的值**：
```cpp
Vec3f new_pos = m_result.moves.back().position - m_extruder_offsets[m_extruder_id] - plate_offset;
```

此处 `moves.back().position` 就是**上一个已存储移动的终点位置**。由于 G1 移动的处理顺序是 `store_move_vertex()` 在 `process_G1` 末尾调用（即当前移动已处理完毕时才存储），所以 `moves.back()` 始终是"刚刚完成的那一个移动"。

**Cloud 模式替代方案**：在 `store_move_vertex()` 中或在其被调用后，将本次移动的终点位置保存到 `m_last_position`。Seam 检测代码改用 `m_last_position` 替代 `m_result.moves.back().position`。

#### 4.3.2 OptionsZCorrector（类别 A 中的复杂依赖）

`OptionsZCorrector` 用于修正换色/暂停时 G-code 中 Color_change 标记的 Z 高度。它：
1. `set()` 保存当前 move 索引
2. `update()` 复制该 move 到末尾并删除中间的原 move

这是唯一需要**非尾部索引访问**的地方。它执行 `erase(begin + *m_move_id)` — 从向量中间删除。

**Cloud 模式替代方案**：**完全跳过 OptionsZCorrector**。它的作用是让 GUI 中换色标记显示在正确的 Z 高度，对 Cloud 切片无任何影响。换色事件已通过 `TimeMachine::stop_times` 和 `custom_gcode_per_print_z` 独立追踪。

#### 4.3.3 spiral_vase_layers 索引追踪（类别 A）

`spiral_vase_layers` 的 `second.second` 存储的是"最后一个非 seam 移动在 moves 中的索引"（:3012, :3447）：
```cpp
m_result.spiral_vase_layers.back().second.second = m_result.moves.size() - 1 - m_seams_count;
```

**Cloud 模式替代方案**：`spiral_vase_layers` 的索引信息仅被 `GCodeViewer`（GUI）消费，用于渲染螺旋花瓶模式的层边界。Cloud 模式可跳过此计算，或仅保留 `first`（Z 高度）和 `second.first`（起始索引）。

#### 4.3.4 类别 B 三项的详细分析

**B1 — Wipe width/height 修正 (:1313-1318)**：
```cpp
for (GCodeProcessorResult::MoveVertex& move : m_result.moves) {
    if (move.type == EMoveType::Wipe) {
        move.width = Wipe_Width;      // 0.05f
        move.height = Wipe_Height;    // 0.05f
    }
}
```
只有在 `store_move_vertex()` 时不设置 width/height 的 Wipe 移动才需要事后修正。可以直接在 `store_move_vertex()` 前将 `m_width` / `m_height` 设为常量值，消除此遍历。

**B2 — layer_duration 赋值 (:1338-1346)**：
```cpp
for (size_t i = 0; i < m_result.moves.size(); i++) {
    size_t layer_id = size_t(m_result.moves[i].layer_duration);
    m_result.moves[i].layer_duration = layer_id == 1
        ? max(0.f, layer_times[layer_id - 1] - prepare_time)
        : layer_times[layer_id - 1];
}
```
`layer_duration` 在 GUI 中用于按层耗时着色。Cloud 切片只需要 `PrintEstimatedStatistics` 中的汇总时间（`modes[...].time`），不需要逐 move 的层耗时。

**B3 — synchronize_moves (:4374-4383)**：
在 `run_post_process()` 末尾调用，将每个 move 的 `gcode_id` 重新映射到 M73 插入后的实际行号。仅 GUI 的 `GCodeViewer::m_sequential_view.gcode_ids` 使用。

### 4.4 具体改动方案

#### 4.4.1 GCodeProcessor.hpp 改动

```cpp
class GCodeProcessor {
public:
    // 新增：Cloud 模式标志
    void set_cloud_mode(bool enabled) { m_cloud_mode = enabled; }
    bool is_cloud_mode() const { return m_cloud_mode; }

private:
    bool m_cloud_mode = false;

    // Cloud 模式下的轻量追踪（替代 m_result.moves）
    Vec3f   m_cloud_last_position = Vec3f::Zero();
    size_t  m_cloud_move_counter = 0;
    float   m_cloud_max_z = 0.0f;
};
```

同时在 `GCodeProcessorResult` 中新增：
```cpp
float max_z_height = 0.0f;  // Cloud 模式下增量追踪的最大 Z
```

#### 4.4.2 GCodeProcessor.cpp 改动

**改动 1 — `store_move_vertex()` (:4773-4812)**

在函数末尾（push_back 之前）加入 cloud 模式分支：

```cpp
void GCodeProcessor::store_move_vertex(EMoveType type, EMovePathType path_type) {
    // ... 现有代码：计算 m_last_line_id, 处理弧线插值点等 ...

    if (m_cloud_mode) {
        m_cloud_move_counter++;
        m_cloud_last_position = Vec3f(m_end_position[X] + m_x_offset,
                                       m_end_position[Y] + m_y_offset, ...)
                                + m_extruder_offsets[m_extruder_id];
        m_cloud_max_z = std::max(m_cloud_max_z, m_cloud_last_position.z());

        if (type == EMoveType::Seam) m_seams_count++;
        if (type == EMoveType::Color_change || type == EMoveType::Pause_Print) {
            // TimeMachine stop_times 仍正常记录（不依赖 moves）
            for (size_t i = 0; i < ...; ++i) {
                TimeMachine& machine = m_time_processor.machines[i];
                if (machine.enabled)
                    machine.stop_times.push_back({ m_g1_line_id, 0.0f });
            }
        }
        return;  // ← 不存入 moves 向量
    }

    // ... 现有代码：m_result.moves.push_back({...}) ...
}
```

**改动 2 — `process_G1()` (:2958-2998) 和 `process_G2_G3()` (:3394-3435)**

Seam 检测中将 `m_result.moves.back().position` 替换为 `m_cloud_last_position`：

```cpp
// 原代码:
const Vec3f new_pos = m_result.moves.back().position
    - m_extruder_offsets[m_extruder_id] - plate_offset;

// Cloud 模式:
const Vec3f last_pos = m_cloud_mode ? m_cloud_last_position
                                    : m_result.moves.back().position;
const Vec3f new_pos = last_pos - m_extruder_offsets[m_extruder_id] - plate_offset;
```

**改动 3 — `process_tags()` (:2083-2091)**

`m_result.moves.empty()` → `m_cloud_mode ? (m_cloud_move_counter == 0) : m_result.moves.empty()`

`m_result.moves.size() - 1 - m_seams_count` → `m_cloud_move_counter - 1 - m_seams_count`

**改动 4 — `finalize()` (:1310-1362)**

Cloud 模式下跳过 B1（Wipe fix）和 B2（layer_duration）：
```cpp
void GCodeProcessor::finalize(bool post_process) {
    if (!m_cloud_mode) {
        // B1: Wipe width/height fix (仅在桌面模式需要)
        for (auto& move : m_result.moves) { ... }
    }

    // TimeBlock 计算、filament 统计 (两种模式都需要)
    // ...

    if (!m_cloud_mode) {
        // B2: layer_duration 赋值 (仅在桌面模式需要)
        for (size_t i = 0; i < m_result.moves.size(); i++) { ... }
    }

    // update_slice_warnings (两种模式都需要)
    update_slice_warnings();

    if (post_process)
        run_post_process();
}
```

**改动 5 — `run_post_process()` 中的 `synchronize_moves()` (:4766)**

```cpp
if (!m_cloud_mode) {
    synchronize_moves(export_lines);
}
```

**改动 6 — `extract_result()`**

将 cloud 追踪数据写入结果：
```cpp
GCodeProcessorResult&& extract_result() {
    m_result.max_z_height = m_cloud_max_z;
    return std::move(m_result);
}
```

**改动 7 — `initialize()` (:1287) 和 `process_file()` (:1268)**

Cloud 模式下跳过 `m_result.moves.emplace_back(MoveVertex())`（初始哑 move）。

**改动 8 — OptionsZCorrector**

Cloud 模式下 `set()` 和 `update()` 均 no-op。

#### 4.4.3 SliceEngine.cpp 改动

**`export_gcode()` (:1857)**

在创建 `Print` 对象后、调用 `export_gcode()` 前，告知 GCodeProcessor 进入 cloud 模式。需要在 `Print::export_gcode()` 中增加 cloud 模式传递机制。

当前调用链：
```
SliceEngine::export_gcode()
  → Print::export_gcode()
    → GCode::do_export()
      → GCodeOutputStream::write()
        → m_processor.process_buffer()
```

`GCodeProcessor` 实例在 `GCode::do_export()` 内部创建（`GCode.cpp:1752`）。需要在 `GCode` 构造或 `do_export()` 时传入 cloud 标志。

**方案**：通过 `Print::export_gcode()` 的参数或 `GCode` 的成员变量传递 `m_cloud_mode` 标志。

**`run_postprocessing()` (:1960)**

将遍历 moves 的 max-Z 检查改为使用 `result.gcode_result.max_z_height`：
```cpp
// 原代码:
if (!result.gcode_result.moves.empty() && ...) {
    float max_z = result.gcode_result.moves[0].position.z();
    for (const auto& move : result.gcode_result.moves)
        if (move.position.z() > max_z) max_z = move.position.z();
    ...
}

// Cloud 模式:
float max_z = result.gcode_result.max_z_height;
if (max_z > 0.0f && max_z - result.gcode_result.printable_height >= 1e-6) {
    ...
}
```

### 4.5 不修改的部分（保持原有行为）

以下 GCodeProcessor 功能在 Cloud 模式下**完全保留**，因为它们不依赖 `moves` 向量：

| 组件 | 行号 | 说明 |
|------|------|------|
| TimeProcessor / TimeMachine | :1320-1327 | 计算各模式预估时间 |
| update_estimated_times_stats | :1331 | 填充 print_statistics.modes |
| m_used_filaments.process_caches | :1329 | 耗材用量统计 |
| update_slice_warnings | :1358 | 切片警告收集 |
| run_post_process (不含 synchronize_moves) | :4028-4766 | M73 行插入 + 占位符替换 |
| filament_diameters / filament_densities | 多个位置 | 耗材参数收集 |
| custom_gcode_per_print_z | :2062, :2072 | 自定义 G-code 标记 |
| extruder_colors | :2390 | 挤出机颜色 |

这些组件产生的输出（`print_statistics`、`filament_diameters`、`filament_densities`、`warnings`、`total_volumes_per_extruder` 等）已覆盖 Cloud 引擎 `package_output()` 和 `build_statistics()` 的全部需求。

### 4.6 风险评估

| 风险点 | 等级 | 缓解措施 |
|--------|------|---------|
| `m_last_position` 与 `moves.back().position` 语义不一致 | 中 | 需要在所有设置 `m_last_position` 的位置与 `store_move_vertex` 中的位置计算保持同步。Seam 检测依赖精确位置，偏差会导致接缝检测不准 → 切片质量可能受影响。需重点测试 seam 相关场景。 |
| OptionsZCorrector 跳过导致行为差异 | 低 | 已确认换色/暂停信息通过独立通道（TimeMachine::stop_times、custom_gcode_per_print_z）传递，不受影响。 |
| spiral_vase_layers 不完整 | 低 | 仅 GUI 消费，Cloud 模式不需要。 |
| GCode 输出二进制一致性问题 | 低 | G-code 文本输出流（GCodeOutputStream::write）与 moves 收集完全解耦，不受影响。 |
| `process_G2_G3` 弧线移动的 seam 检测 | 中 | 与 process_G1 相同模式，同样替换为 `m_cloud_last_position`，需覆盖弧线移动的测试。 |
| Wipe width/height 不修正的影响 | 低 | 仅在 GUI 渲染中使用，不影响 G-code 文本输出和打印质量。 |

### 4.7 改动量估算

| 文件 | 改动行数 | 改动类型 |
|------|---------|---------|
| `GCodeProcessor.hpp` | ~25 行 | 新增 cloud 模式标志和追踪字段 |
| `GCodeProcessor.cpp` | ~60 行 | store_move_vertex 分支、finalize 条件跳过、seam 检测替换 |
| `GCodeProcessor.cpp` (OptionsZCorrector) | ~15 行 | cloud 模式 no-op |
| `GCode.hpp / GCode.cpp` | ~10 行 | cloud 标志传递（构造函数或成员） |
| `Print.hpp / Print.cpp` | ~5 行 | export_gcode 参数透传 |
| `SliceEngine.hpp` | ~3 行 | max_z_height 字段或 cloud 标志 |
| `SliceEngine.cpp` | ~15 行 | max-Z 检查改用 max_z_height + 设置 cloud 模式 |
| **合计** | **~133 行** | |

### 4.8 内存节省估算

对于醒狮眼镜仔_木公 19MB 版本（按 5700 万 moves 估算）：

| 数据 | 当前 | Cloud 模式 |
|------|------|-----------|
| `moves` 向量 | ~8.6 GB | ~100 bytes |
| `lines_ends` 向量 | ~450 MB | 0 |
| 其他元数据 | ~50 MB | ~50 MB |
| **G-code 导出阶段峰值** | **~9.1 GB** | **~50 MB** |
| **叠加 Support Gen 后的总峰值** | **~15 GB** | **~6.5 GB** |

Pod 限制 16 GB 的情况下：原来超出 16 GB，Cloud 模式后峰值仅 ~6.5 GB，**余量 9.5 GB**。

---

## 五、结论与建议

### 5.1 方案对比

| 维度 | 方案 1（释放已完成 plate） | 方案 2（Cloud Mode） |
|------|--------------------------|---------------------|
| 解决层 A（单 plate OOM） | ❌ | ✅ |
| 解决层 B（多 plate 累积） | ✅ | ✅ |
| 改动量 | ~20 行 | ~133 行 |
| 风险 | 极低 | 中等（需重点测试 seam 检测） |
| 涉及文件数 | 3 | 7 |
| 对桌面版影响 | 无 | 无（cloud mode 是可选标志） |

### 5.2 建议实施路径

1. **先实施方案 1**（1-2 天）— 快速解决多 plate 场景的内存累积问题，改动极小，零风险
2. **然后实施方案 2**（3-5 天）— 根本性解决单 plate OOM 问题，需配合充分的 seam 回归测试
3. 两个方案可以独立部署，方案 1 作为方案 2 的前置优化

### 5.3 方案 2 的测试要点

- **Seam 位置准确性**：对比 Cloud 模式和桌面模式同一模型的 seam 位置
- **弧线移动**：G2/G3 移动的 seam 检测是否正常
- **螺旋花瓶模式**：确认不影响 G-code 文本输出
- **多挤出机**：`m_extruder_offsets` 偏移计算一致性
- **内存峰值**：醒狮眼镜仔 3 个版本的内存曲线对比
- **输出二进制一致性**：G-code 文件内容与桌面版逐字节对比（不含时间戳行）
