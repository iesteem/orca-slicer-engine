# P-16: inherits_group 缺格式断言 → 降级 warning（parity 决策）

**状态：** 2026-09-16 审查报告 C4；2026-09-18（66 agent）+ 2026-09-20
（复核）两轮对抗审查确认"仍存在但**有意不修**"。此前该降级决策无文档
记录，本判例补记——**下轮审查勿再重开此条**。

## 现象

引擎解析 `inherits_group` 时 Guard 2（`SliceEngine.cpp:1246-1279`）只做
越界检查（`parent_idx < inherits_size`），**不断言**
`inherits_size == filament_count + 2`。短数组/旧格式 3MF 理论上可触发
槽位错位解析（触发前提——真实旧格式短数组 3MF——原报告即标注未证实）。

## 布局契约（引擎与桌面一致）

写入端（桌面 `Snapmaker_Orca.cpp:2432`）：`resize(filament_count + 2)`，
布局 `[process, fil₁..fil_N, printer]`。

| 引擎读取端 | 索引 | 防护 | 主线对应 |
|---|---|---|---|
| filament Case 0（:1256-1260） | `[i+1]` | 有界 + 非空 + 系统查找 + 官方 vendor（Guard 2-5） | `PresetBundle.cpp:1269` `[index+1]` |
| printer（:1026） | `back()` | 空检查 | `Snapmaker_Orca.cpp:1474` `[size-1]`（**无空检查**） |
| process（:1412） | `[0]` | 空检查 | `Snapmaker_Orca.cpp:1483` `[0]` |

主线读取端 `PresetBundle.cpp:1254` 同样只 `resize(N+2)` + 按位索引、
无格式校验（resize 本身即越界防护：短补空串 = "该槽 official"语义）。
引擎短数组时 Guard 2 fall through → Case 1 直查名字——与主线补空串
行为**同向**，均不 hard-fail。引擎三处读取端全部有空/界防护，防御
强于主线。

## 为什么不修

加 `N+2` 断言会让引擎在主线 resize 语义可容忍的短数组上直接 fail，
制造桌面没有的行为分叉（违反 parity 章程）。触发前提未证实；残余
风险 = 短数组 3MF 走 Case 1/2/rollback（= 桌面行为），最坏回退通用
预设，不崩溃不 UB。

## 已证伪的推测（勿重走弯路）

- **"主线读取端裸索引"不准确**：`PresetBundle` 的 `resize(N+2)` 是
  防护手段而非缺陷；真正裸索引 UB 在 GUI 端
  `Snapmaker_Orca.cpp:1474/1492`（`size==0` → `[size-1]` 下溢、
  `resize(size-2)` 下溢）——主线真缺陷，但云引擎不链接 GUI 文件，
  不影响引擎。
- **trim 错位通道已消失**：引擎曾有 per-plate trim 的 slot remap
  （把实际使用槽位重写到 index 0）会令 `[i+1]` 错位；trim 已整体
  移除（`SliceEngine.cpp:2421-2438` 注释），resolve 循环
  （:1155-1159）遍历原始 `filament_settings_id`。
- **基线漂移排除**：cac2f98..HEAD 对 `SliceEngine.cpp` 共 1593 行
  增删中 Guard 2 区域零改动（逐字节同基线）；95a2246（3N+7C）修复
  清单不含 C4；tests/ 无短数组回归用例（缺口随本判例接受）。
