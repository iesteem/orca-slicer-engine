# G-code 导出时 CPU 核数与内存峰值的关系分析

## 概述

orca-slice-engine 使用 Intel TBB (Threading Building Blocks) 进行并行计算。线程数由 `--threads <N>` 参数控制，底层通过 `tbb::global_control::max_allowed_parallelism` 限制 TBB 线程池大小。

G-code 导出涉及两个主要阶段，每个阶段的并行度与内存峰值的关系不同。

---

## 1. 并行架构概览

```
SliceEngine::run()
  │
  ├── tbb::global_control(thread_count)    ← 全局线程数控制
  │
  ├── [Phase 1] print.process()            ← 切片阶段
  │   ├── tbb::parallel_for on m_layers    ← 逐层并行 (PrintObject 内部)
  │   │   ├── make_perimeters()            ← 每层分配 perimeter 路径
  │   │   ├── make_fills()                 ← 每层分配 infill 路径
  │   │   ├── make_ironing()               ← 每层分配 ironing 路径
  │   │   └── simplify_extrusion_path()    ← 每层简化路径
  │   │
  │   └── tbb::parallel_for on m_objects   ← 逐对象并行 (Print 级别)
  │       └── generate_support_material()  ← 支撑材料生成 (内存大户)
  │
  └── [Phase 2] print.export_gcode()       ← G-code 导出阶段
      └── tbb::parallel_pipeline(12, ...)  ← 流水线，固定 12 tokens
          ├── generator                    ← 生成每层 G-code (LayerResult)
          ├── spiral_mode / pressure_equalizer  ← 后处理过滤器
          ├── cooling                      ← 冷却缓冲区
          ├── fan_mover                    ← 风扇控制
          └── output                       ← 写入文件
```

### libslic3r 中的 TBB 使用统计

整个 libslic3r 库中有 **143 处** `tbb::parallel_for` / `tbb::parallel_pipeline` 调用，分布在：

| 文件 | 并行调用数 | 并行粒度 |
|------|-----------|---------|
| `PrintObject.cpp` | ~25 | 按层 (m_layers) |
| `Print.cpp` | ~7 | 按对象 (m_objects) |
| `GCode.cpp` | 8 | 流水线 (固定 12 tokens) |
| `MultiMaterialSegmentation.cpp` | ~8 | 按层 |
| `Brim.cpp` | 2 | 按多边形环 |
| 其他 | ~93 | 网格/面片等 |

---

## 2. 切片阶段 (print.process) 的内存-线程关系

### 2.1 逐层并行 (PrintObject 内部)

`PrintObject` 中几乎所有耗时操作都是按层并行化的：

```cpp
// PrintObject.cpp:385 — 生成 perimeters
tbb::parallel_for(
    tbb::blocked_range<size_t>(0, m_layers.size()),
    [this](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_layers[layer_idx]->make_perimeters();  // ← 每层分配新内存
        }
    }
);

// PrintObject.cpp:550 — 生成 infill
tbb::parallel_for(
    tbb::blocked_range<size_t>(0, m_layers.size()),
    [this, ...](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_layers[layer_idx]->make_fills(...);  // ← 每层分配新内存
        }
    }
);
```

**内存影响**：
- 每层的 `make_perimeters()` 和 `make_fills()` 会为 ExtrusionPath、ExtrusionEntity 等对象分配堆内存
- TBB 将 `[0, m_layers.size())` 按 `blocked_range` 分块，每个线程处理一个 chunk
- **线程数 = N 时，最多 N 个层同时在做内存分配**
- 一个复杂模型的单层 perimeter + infill 数据可达数 MB
- **峰值内存 ≈ N × 单层工作内存 + 所有层的最终数据**

### 2.2 逐对象并行 (Print 级别)

```cpp
// Print.cpp:2497 — 支撑材料生成跨对象并行
tbb::parallel_for(tbb::blocked_range<int>(0, int(m_objects.size())),
    [this, need_slicing_objects](const tbb::blocked_range<int>& range) {
        for (int i = range.begin(); i < range.end(); i++) {
            PrintObject* obj = m_objects[i];
            if (need_slicing_objects.count(obj) != 0) {
                obj->generate_support_material();  // ← 内存大户
            }
        }
    }
);
```

**内存影响**：
- `generate_support_material()` 内部又有自己的 `tbb::parallel_for`（逐层支撑生成）
- 多个对象同时生成支撑 → 内存叠加
- 线程数越多，同时生成支撑的对象数越多
- **支撑材料是切片阶段最大的单一内存消费者**（尤其是 tree/organic support）

### 2.3 线程数对切片阶段的内存影响

| 线程数 | 同时处理的层数 | 同时生成支撑的对象数 | 内存峰值趋势 |
|--------|--------------|-------------------|-------------|
| 1 | 1 层 | 1 对象 | 最低 |
| 2 | ~2 层 | ~2 对象 | ~1.5-2× |
| 4 | ~4 层 | ~4 对象 | ~2-3× |
| 8 | ~8 层 | ~8 对象 | ~3-5× |
| 16+ | ~16 层 | ~16 对象 | 接近上限 |

> **注意**：不是严格的线性关系，因为不同层的复杂度差异大，且 TBB 的工作窃取机制会使实际并行度略低于线程数。

---

## 3. G-code 导出阶段 (export_gcode) 的内存-线程关系

### 3.1 流水线架构

```cpp
// GCode.cpp:3187-3193 — 固定 12 tokens 的并行流水线
tbb::parallel_pipeline(12, generator & cooling & fan_mover & output);
```

关键数据结构 `LayerResult`：
```cpp
// GCode.hpp:171-183
struct LayerResult {
    std::string gcode;            // ← 单层的完整 G-code 文本
    size_t      layer_id;
    bool        spiral_vase_enable;
    bool        cooling_buffer_flush;
    bool        nop_layer_result;
};
```

### 3.2 流水线内存占用分析

流水线有 5-6 个阶段：

```
generator → spiral_mode → pressure_equalizer → cooling → fan_mover → output
   ↓             ↓              ↓                ↓          ↓          ↓
 LayerResult  LayerResult    LayerResult      std::string std::string   void
```

- **12 tokens** 是流水线中同时在飞的 token 上限
- 每个 token 在 `generator → cooling` 阶段是一个 `LayerResult`（含完整 G-code 文本字符串）
- **G-code 文本大小**：一个复杂层可达 10-50 MB（取决于模型复杂度和 infill 密度）
- **最坏情况**：12 个 tokens × 50 MB/token = **600 MB** 仅在流水线中

### 3.3 线程数与流水线的关系

- `tbb::parallel_pipeline(12, ...)` 的 **12 是写死的常量**，不随线程数变化
- 流水线中的每个 filter 可以在不同线程上运行
- 线程数越多，token 在各阶段间的流动越快，但 **同时在飞的 token 数上限始终是 12**
- 线程数不影响流水线的峰值内存（token 数固定），但影响：
  - `generator` 阶段 `process_layer()` 内部的并行度
  - cooling buffer / fan mover 内部可能使用的线程数

### 3.4 process_layer 内部

```cpp
// GCode.hpp:343 — 单层 G-code 生成
LayerResult process_layer(
    const Print& print,
    const std::vector<LayerToPrint>& layers,
    const LayerTools& layer_tools,
    bool last_layer, ...);
```

`process_layer()` 本身是串行的（在流水线中作为 serial_in_order filter 运行），
但它访问的 Layer 数据（perimeters、infill paths）已在切片阶段分配好。

### 3.5 cooling buffer 的额外内存

```cpp
// cooling_buffer.process_layer() 缓存多层的 G-code
// 用于在层间插入温度等待命令
```

Cooling buffer 会缓存若干层的 G-code 文本，增加了额外的内存占用。

---

## 4. 关键结论

### 4.1 CPU 核数与内存峰值的关系

```
                    切片阶段                           G-code 导出阶段
线程数 ──→ 同时处理的层/对象数 ──→ 内存峰值    线程数 ──→ 流水线吞吐 ──→ 内存峰值
              ↑ 线性相关                                          ↑ 弱相关
         (每层独立分配内存)                              (token 数固定=12)
```

1. **切片阶段**（`print.process()`）：线程数与内存峰值呈 **正相关**。
   - 每增加一个线程，就多一层/一个对象的工作内存同时存在
   - 在有多个 PrintObject 的盘上，影响更大（所有对象共享线程池）
   - 预估：从 1 线程到 N 线程，切片阶段峰值内存增长约 **1.5× ~ 3×**（取决于对象数）

2. **G-code 导出阶段**（`print.export_gcode()`）：线程数与内存峰值呈 **弱相关**。
   - 流水线 token 数固定为 12，主要由 G-code 文本大小决定
   - 线程数增加只加速 token 流过，不增加同时在飞的 token 数
   - **真正决定 G-code 导出内存的是模型复杂度而非线程数**

3. **总体峰值内存** = 所有层的最终数据（~常量）+ 并行工作内存（~随线程数增长）+ 流水线 token 内存（~常量）

### 4.2 低配机 OOM 的原因

低配机 (少 CPU + 少 RAM) OOM 不是单纯因为"线程少导致某种内存放大"，而是：

1. **绝对内存不足**：libslic3r 的 Layer 数据结构（所有层的 perimeters、infill、support）本身就需要大量内存，与线程数无关
2. **G-code 流水线 token 数为硬编码 12**：即使只有 2 个 CPU 核，流水线仍允许 12 个 LayerResult（各含完整 G-code 文本）同时存在
3. **swap 导致更慢释放**：内存紧张时系统 swap → 处理变慢 → 内存持有时间更长 → 叠加更多内存

### 4.3 优化建议

| 优化项 | 预期效果 | 实现难度 |
|--------|---------|---------|
| 将 `parallel_pipeline` token 数从 12 降到可配置值 | G-code 导出内存降低 30-50% | 低 |
| `--threads 1` 序列化切片阶段 | 切片阶段内存降低 30-50% | 已支持 |
| 切片完成后主动释放不需要的 Layer 数据 | 大幅降低导出前基线内存 | 中 |
| 将 `moves` / `lines_ends` 等可视化数据不生成 | 减少 100-500 MB (已做) | 已实现 |
| 流水线中将 G-code 直接写文件而非缓存在 string | 消除 token 内存累积 | 中 |

### 4.4 推荐的 thread_count 设置

对于不同内存大小的机器：

| 机器内存 | 推荐 --threads | 原因 |
|---------|---------------|------|
| < 2 GB | 1 | 避免并行工作内存叠加 |
| 2-4 GB | 2 | 适度并行 |
| 4-8 GB | 4 | 较好并行度 |
| > 8 GB | 0 (all cores) | 内存充裕 |

---

## 5. 可进一步验证的方法

1. **用 `--threads` 做对照实验**：分别设置 1/2/4/8/0，监控 `/usr/bin/time -v` 的 Maximum resident set size
2. **在 `SliceEngine.cpp:242` 附近添加内存日志**：在 TBB control 创建前后打印 RSS
3. **在 `GCode.cpp:3187` 附近添加日志**：打印流水线中 `LayerResult::gcode.size()` 的值
4. **使用 heaptrack / massif** 分析堆内存分配热点
