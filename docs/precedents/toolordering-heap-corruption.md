# P-10: 5479 ToolOrdering 堆损坏（上游 bug，搁置）

**状态：** 2026-08-20 用户决定暂不处理
**Fixture：** `~/Downloads/5479_71a4b623998e7419.3mf`（外部，未入库）

## 现象

`ToolOrdering::fill_wipe_tower_partitions` 内 raft-gap insert 分支的
`lt_extra.extruders.push_back` 触发 `_M_realloc_insert` SIGSEGV (139)。
**桌面版 OrcaSlicer 同模型同崩** → 上游 libslic3r bug，非引擎问题。

## 已证伪的修复（勿重走）

- ❌ `lt_object` 引用改值拷贝——该引用 insert 后未被写，无效
- ❌ insert 后刷新全部 `WipingExtrusions::m_layer_tools` back-pointer
  （悬垂假设）——改动确认进二进制，崩溃不变

关键现场：崩溃时 `lt_extra` 首字段 `print_z = 0xCCCCCCCD`（乱码）→
**堆在进入该函数前已被写坏**，此处只是第一个撞上的分配。触发条件疑似：
擦除塔 + 大首层间隙 + 树支撑 auto-lift 组合。

## 下次处理路径

ASan：上游 CMakeLists.txt:400 已有 `-fsanitize=address` 开关，建独立
build 目录（build-engine-asan）重编 libslic3r + 引擎跑 5479。上游源码
留有两个未提交的防御性补丁（ToolOrdering.cpp，无害）。
