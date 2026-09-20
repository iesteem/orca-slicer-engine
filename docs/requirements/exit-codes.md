# 需求：退出码与 Issue Code 契约

> 来源实现：`src/Types.hpp`（EXIT_* 常量）、`src/SliceEngine.cpp`（issue
> 发射点）。**[anchor] 级契约**：调用方以退出码做流程分支，以 issue code
> 做用户呈现，二者均不可漂移。

## 退出码

| 码 | 常量 | 语义 | 典型来源 |
|---|---|---|---|
| 0 | EXIT_OK | 全部成功（含 gcode.3mf 打包）；多盘**部分成功**（其余盘为良性空盘跳过）同样返回 0（P-19） | 正常切片 |
| 1 | EXIT_INVALID_ARGS | CLI 参数无效 | main.c |
| 2 | EXIT_FILE_NOT_FOUND | 输入文件不存在 | main.c |
| 3 | EXIT_LOAD_ERROR | 3MF 加载/格式失败 | FORMAT_REJECTED、LOAD_3MF_ERROR 类 |
| 4 | EXIT_SLICING_ERROR | 切片阶段失败（某盘或整体） | SLICING_* 类 |
| 5 | EXIT_EXPORT_ERROR | G-code 导出失败 | GCODE_EXPORT_ERROR |
| 6 | EXIT_PREPROCESS_ERROR | 切片前校验失败（config/输入/预设）；**零产物守卫**（所有请求盘被跳过/失败，无任何产物时兜底非零，P-19；不遮蔽已记录的更具体码） | 预设替换、几何校验、print_sequence 阻断、零产物 |
| 7 | EXIT_POSTPROCESS_ERROR | 切片成功但产物不可用 | TOOLPATH_OUTSIDE、空层 |

139 = SIGSEGV，**不是合法退出码**，出现即引擎缺陷（见判例 P-01/P-02/P-09/P-10）。

## Issue level 四级

`error`（阻断，决定退出码）/ `serious_warning` / `warning` / `tip`。
注意：几何缺陷检查（GEOM_*）历史上**全为 warning 不阻断**，
`has_geom_error` 阻断分支是死代码（判例 P-12）。

## Issue Code 全表（65 个，按域分组）

### 输入/加载（→ exit 3）

| Code | Level | 说明 |
|---|---|---|
| FORMAT_REJECTED | error | 扩展名/格式拒绝（.4mf、.zip 等） |
| LOAD_3MF_ERROR* | error | read_from_file 抛异常 / empty 检测（注意：no_model_objects 实测归此类而非 MODEL_EMPTY） |
| FILE_SIZE_EXCEEDED | error | 超 max_size_mb |
| MODEL_EMPTY | error | 模型无对象 |
| PLATE_NOT_FOUND | error | 指定盘不存在 |
| NO_PRINTABLE_OBJECTS | warning | 盘上无可打印对象（apply 后归零）——良性跳过不阻断（P-19；本行级别曾误标 error，2026-09-20 对齐代码实测 make_warning） |
| EMPTY_PLATE | warning | 盘无任何可打印实例，良性跳过（P-19）；零产物场景由 run() 零产物守卫兜底 exit 6 |

### 预设/配置（→ exit 6，部分 warning）

| Code | Level | 说明 |
|---|---|---|
| PRESETS_MISSING | error | system presets 加载失败（默认路径干净失败；配 --skip-preset-substitution 会崩，见 P-09） |
| PROCESS_SUBSTITUTED / PROCESS_NOT_SUBSTITUTED | mixed | 工艺预设替换结果 |
| PRINTER_SUBSTITUTED / PRINTER_PRESET_NOT_APPLIED / PRINTER_PRESET_LOAD_ERROR | mixed | 打印机预设 |
| PRINTER_MODEL_MISSING / PRINTER_MODEL_UNSUPPORTED | error | 非 Snapmaker U1 拒绝 |
| FILAMENT_SUBSTITUTED / FILAMENT_ROLLED_BACK / FILAMENT_UNKNOWN / FILAMENT_UNKNOWN_ANCESTOR / FILAMENT_NO_OFFICIAL_ANCESTOR / FILAMENT_CIRCULAR_INHERITS / FILAMENT_UNSUPPORTED_VENDOR | mixed | 耗材预设替换/回退链 |
| FILAMENT_TEMP_MIXING | error | 高低温耗材混用 |
| FILAMENT_BED_GESP_ADHESION / FILAMENT_BED_PEI_ADHESION / FILAMENT_BED_PEI_TPU_STICKING | warning | 床面粘附提示 |
| FILAMENT_NOZZLE_MISMATCH / BED_FILAMENT_MISMATCH | warning | 喷嘴/床面耗材匹配 |
| CONFIG_SUBSTITUTION / CONFIG_UNRECOGNIZED | warning | 配置键替换/未识别 |
| CONFIG_INVALID_<key> | warning | 动态拼接：非法配置值（键名后缀可变，勿做精确枚举断言） |

### 几何预检（全 warning，不阻断 — P-12）

GEOM_SELF_INTERSECT / GEOM_MULTI_COMPONENT / GEOM_ZERO_VOLUME /
GEOM_NON_MANIFOLD / GEOM_INVERTED / GEOM_EMPTY / GEOM_DEGENERATE；
OBJECT_BELOW_BED_ADJUSTED（全沉床自动抬升）、
OBJECT_INTENTIONALLY_BELOW_BED（有意下沉保留）。

### build-volume / 校验阻断（→ exit 6）

BUILD_VOLUME_FULLY_OUTSIDE（error）、PRINT_BY_OBJECT_CAUTION（U1 禁逐件
打印，盘级覆盖可救）、TIMELAPSE_BY_OBJECT / TIMELAPSE_SPIRAL_VASE、
ORGANIC_SUPPORT_VARIABLE_LAYER_HEIGHT / PRIME_TOWER_VARIABLE_LAYER_HEIGHT。

### 切片（→ exit 4）

SLICING_ERROR / SLICING_EXCEPTION / SLICING_FATAL_ERROR /
SLICING_TIMEOUT / SLICING_CANCELLED / PRINT_EMPTY_GCODE_LAYERS /
PRINT_GCODE_OVERLAP / WIPE_TOWER_TOOLCHANGE_ERROR / PRINT_WARNING(_CRITICAL)。

### 导出/后处理（→ exit 5 / 7）

GCODE_EXPORT_ERROR / PACKAGE_EXPORT_ERROR（exit 5）；
TOOLPATH_OUTSIDE / TOOL_HEIGHT_OUTSIDE / TOOLPATH_CONFLICT /
SPIRAL_LIFT_NEAR_BOUNDARY（exit 7）。

### 安全/其他

USER_CONTENT_CLEARED（tip：用户 notes/外链剥离）、INTERNAL_ERROR /
INTERNAL_FATAL（引擎内部缺陷，出现即 bug）。

## 契约规则

1. 新增 code 需同步本表 + CHANGELOG；renamed code 视同 breaking change。
2. code → exit 映射由发射点所在阶段决定（上表为实测归纳），断言以实测
   为准（fixture 文件名编码预期，见 tests/fixtures/）。
3. `plate_id` 全局级 issue 为 -1，盘级 1-based（json-output.md）。

*LOAD_3MF_ERROR 为本文归纳名，实现处为动态 code（SliceEngine.cpp:447）。
