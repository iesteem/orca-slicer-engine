# 3MF 预处理方案

> 解决 3MF 跨版本兼容性问题，使任意来源的 3MF 文件能通过云引擎验证

---

## 一、问题背景

云切片引擎（orca-slice-engine）基于特定版本的 libslic3r，对 3MF 内嵌的切片配置有严格校验。用户从不同版本 OrcaSlicer、Bambu Studio 等工具导出的 3MF 可能包含：

- **值超出范围**：如 `tree_support_wall_count = -1`（其他版本中表示"自动"，引擎仅接受 `[0, 2]`）
- **不可识别的键**：如 `apply_scarf_seam_on_circles`（新版本引入，旧引擎不认识）
- **缺少必要字段**：如未设 `printer_model = "Snapmaker U1"`
- **自定义预设引用**：如 `Snapmaker PLA @U1` 用户克隆后继承链断裂

直接传入引擎 → exit 6 验证失败。

## 二、解决方案

在引擎前增加预处理层，利用 `--dump-config-schema` 获取引擎能力清单，对每个 3MF 自动修复。

```
用户 3MF → [预处理层] → 引擎兼容 3MF → orca-slice-engine → G-code
                ↑
     --dump-config-schema (引擎能力清单)
```

## 三、引擎能力清单

```bash
orca-slice-engine --dump-config-schema > schema.json
```

输出 660 个配置键的完整元数据：

```json
{
  "tree_support_wall_count": {"type": "int", "min": 0, "max": 2, "label": "Support wall loops"},
  "sparse_infill_density":  {"type": "percent", "min": 0, "max": 100},
  "bottom_surface_pattern": {"type": "enum", "values": ["monotonic","monotonicline",...]},
  "gcode_flavor":           {"type": "enum", "values": ["marlin","klipper","reprapfirmware","marlin2"]},
  ...
}
```

## 四、预处理规则

### 规则 1：删除不可识别的键

不在 schema 中的键直接删除（引擎报 `CONFIG_UNRECOGNIZED` 警告，不致命，但建议清理）。

### 规则 2：数值 clamp 到合法范围

对照 schema 的 `min` / `max`，超出范围的值 clamp。

| 配置键 | 范围 | 常见违规值 | 处理 |
|--------|------|-----------|------|
| `tree_support_wall_count` | [0, 2] | -1 | → 0 |
| `sparse_infill_density` | [0, 100] | — | clamp |
| `wall_loops` | [0, 1000] | — | clamp ≥ 0 |
| `bridge_flow` | (0, 2] | 0 | → 1.0 |
| `*_line_width` 系列 | ≤ 5× nozzle_diameter | 过大值 | clamp 上限 |

### 规则 3：枚举值校验

对照 schema 的 `enum_values`，非法值替换为该键的默认值。

| 配置键 | 合法值 |
|--------|--------|
| `gcode_flavor` | marlin, klipper, reprapfirmware, marlin2 |
| `sparse_infill_pattern` | 26 种（rectilinear, grid, gyroid, ...） |
| `top_surface_pattern` | 8 种（monotonic, concentric, ...） |

### 规则 4：注入缺失的必要字段

| 字段 | 值 | 原因 |
|------|-----|------|
| `printer_model` | `"Snapmaker U1"` | 引擎硬校验，其他值 → exit 6 |

### 规则 5：始终传递 CLI 宽容标志

```bash
--allow-custom-filament-presets
--allow-custom-printer-presets
--max-size 0
```

## 五、参考实现

```python
import json, zipfile, subprocess, io, shutil, os, sys

def load_schema(engine_path):
    """获取引擎能力清单"""
    result = subprocess.run(
        [engine_path, "--dump-config-schema"],
        capture_output=True, text=True
    )
    return json.loads(result.stdout)

def preprocess_3mf(src_path, dst_path, schema):
    """预处理单个 3MF 文件"""
    with zipfile.ZipFile(src_path, 'r') as zin:
        # 读取配置
        config_raw = zin.read('Metadata/project_settings.config').decode('utf-8', errors='replace')
        config = json.loads(config_raw)

        # 规则 1: 删除不可识别的键
        for key in list(config.keys()):
            if key not in schema:
                del config[key]

        # 规则 2: clamp 数值
        for key, val in config.items():
            if key not in schema:
                continue
            s = schema[key]
            t = s['type']
            try:
                if t in ('int', 'ints', 'percent', 'percents'):
                    v = int(val)
                    if 'min' in s and v < s['min']: config[key] = str(s['min'])
                    if 'max' in s and v > s['max']: config[key] = str(s['max'])
                elif t in ('float', 'floats', 'float_or_percent', 'floats_or_percents'):
                    v = float(val)
                    if 'min' in s and v < s['min']: config[key] = str(s['min'])
                    if 'max' in s and v > s['max']: config[key] = str(s['max'])
            except (ValueError, TypeError):
                pass

        # 规则 3: 枚举值校验
        for key, val in config.items():
            if key not in schema:
                continue
            s = schema[key]
            if s['type'] in ('enum', 'enums') and 'enum_values' in s:
                if val not in s['enum_values']:
                    config[key] = s['enum_values'][0]  # 用第一个值兜底

        # 规则 4: 注入缺失的必要字段
        config.setdefault('printer_model', 'Snapmaker U1')

        # 写出修复后的 3MF
        with zipfile.ZipFile(dst_path, 'w', zipfile.ZIP_DEFLATED) as zout:
            for item in zin.infolist():
                if item.filename == 'Metadata/project_settings.config':
                    zout.writestr(item, json.dumps(config))
                else:
                    zout.writestr(item, zin.read(item.filename))

    return dst_path

def slice_with_engine(input_3mf, output_prefix, engine_path, resources_path):
    """调用引擎切片"""
    schema = load_schema(engine_path)
    fixed = preprocess_3mf(input_3mf, input_3mf.replace('.3mf', '_fixed.3mf'), schema)

    result = subprocess.run([
        engine_path,
        '-v', '-j',
        '-o', output_prefix,
        '-r', resources_path,
        '--allow-custom-filament-presets',
        '--allow-custom-printer-presets',
        '--max-size', '0',
        fixed
    ], capture_output=True, text=True)

    # 读取统计 JSON
    stats_path = f'{output_prefix}.json'
    if os.path.exists(stats_path):
        with open(stats_path) as f:
            stats = json.load(f)
        return stats

    return {'success': False, 'error': result.stderr}

# ---- 使用示例 ----
if __name__ == '__main__':
    stats = slice_with_engine(
        input_3mf='model.3mf',
        output_prefix='output',
        engine_path='./orca-slice-engine',
        resources_path='./resources'
    )
    print(json.dumps(stats, indent=2, ensure_ascii=False))
```

## 六、效果验证

以 `d23b2011c04a983b.3mf` 为例：

| 阶段 | exit code | 原因 |
|------|-----------|------|
| 预处理前 | 6 | `tree_support_wall_count = -1` 超出 [0,2] |
| 预处理后 | 5 | 通过配置验证，剩余失败来自模型几何问题（自相交 mesh） |

预处理修复了 8 处配置问题（1 个 clamp + 7 个未知键删除），3MF 成功通过引擎配置验证。

## 七、维护

当引擎升级（libslic3r 版本更新导致配置定义变化）时，只需重新执行：

```bash
orca-slice-engine --dump-config-schema > schema_v0201XX.json
```

预处理代码无需改动，schema 文件自动反映最新规则。
