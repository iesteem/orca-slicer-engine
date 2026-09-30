# 项目宪章（CHARTER）

> 本文件回答"这个项目为何存在"，是全仓库寿命最长的文档。代码可以整体重写，
> 本文件描述的边界与决策不随之改变。修改本文件需要人类明确批准。

## 存在理由

orca-slice-engine 是 OrcaSlicer 的 **headless 切片引擎**：吃 3MF，吐 gcode /
gcode.3mf + JSON 报告，无 GUI。它存在的目的是让云端批量切片获得与桌面版
OrcaSlicer **行为一致**的结果，而不是重新发明一个切片器。

## 权威关系

- **桌面版 OrcaSlicer 是行为准绳。** 当引擎与桌面版行为不一致时，除非有
  明确记录的偏离决策，否则一律视为引擎的 bug，以对齐桌面版为修复方向。
  对齐基准版本在 CHANGELOG 中记录（当前锚定 2.3.5-base 的行为判例）。
- 上游 libslic3r / OrcaSlicer 的 bug 属于外部沉疴：记录于判例库，不盲目
  在引擎侧"修补"，除非有绕过方案且记录了根因。

## 边界（不做什么）

- 不移植任何 GUI 逻辑（预览、手动调整、交互式修复）。
- 不做交互式 API：引擎一次性运行、输出、退出。
- 不提供 flow type（流量类型）选择 API —— `filament_volume_type`
  透传即是终态（见判例 P-xx）。

## 已确认的设计决策（宪法级，改动需人类批准）

| 决策 | 内容 | 状态 |
|---|---|---|
| all-or-nothing 打包 | 任一盘失败则不产出 gcode.3mf（即使 10/11 盘成功） | 用户确认保持现状（SliceEngine.cpp:224 附近） |
| JSON 输出契约 | `plates[].filaments[].id`（非 `filament_id`） | v02.01.10 起 |
| 单一构建模式 | 仅 ORCA_STATIC，静态链接全部依赖 | Consumer 模式已移除 |
| C ABI 边界 | 7 个纯 C 导出，零 C++ 类型泄漏 | 设计约束 |

## 文档体系（灵魂层）

本仓库按"寿命分层"组织文档，代码（肉体）可在下次整体重构时完全抛弃，
以下各层必须被继承：

1. `CHARTER.md` —— 本文件
2. `METHODOLOGY.md` —— 方法论原则
3. `docs/requirements/` —— 分模块需求/用例
4. `docs/precedents/` —— 行为判例库（轮回的"不死判官"）
5. `CHANGELOG.md` —— 更新日志
6. 数据与 fixture —— 完全被继承
