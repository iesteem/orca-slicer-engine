# P-14: 构建与环境约束（强制）

## 本机硬约束

1. **所有编译命令必须 `-j 2`**——否则占满资源导致本机卡死。
2. **Orca 分支切换后必须强制重建 libslic3r.a**：
   ```bash
   cd build-engine && ninja src/libslic3r/liblibslic3r.a src/libslic3r/liblibslic3r_cgal.a
   ```
   （目标名是 `liblibslic3r.a` 不是 `libslic3r.a`）。否则旧归档混链
   （mtime 未变 ninja 不重编，gen_static_libs.sh 不检测陈旧）→ ODR/ABI
   混链 → all_regions() 等处 SIGSEGV。
   build-engine 里裸跑 `ninja` 会编 252 个含 GUI 的目标（缺 wx 必失败），
   只编 libslic3r 目标。

## 无害噪音（不是回归信号）

- `get_hrc_by_nozzle_type: parse .../nozzle_info.json parse_error` ——
  无头环境路径拼接找不到文件，`Print.cpp:3074` catch 有硬编码 fallback
  表（hardened_steel=55 等），HRC 照常返回。2026-08-20 决定不修。

## bundle 出厂自检（C5 闭环，2026-09-20）

package/ 自 25ac1b6 起不进 git（.gitignore），生产 bundle 由
scripts/package_linux.sh 打包时从 fork（ORCA_ROOT/resources，git 检出）
实时拷贝、无独立锁定；引擎侧 system preset 加载失败是整单致命（P-09）。
因此打包脚本在 profiles 拷贝后内嵌 python3 自检，任一失败即中止打包：
已实例化 filament 预设沿 inherits 链合并后必须有 filament_vendor、链
不得悬空/成环、JSON 必须可解析、空 bundle 拒绝放行（防空目录假绿）。
红绿矩阵已验：删 `Snapmaker Support For PLA @U1 base`（真实 C5 场景，
67 个预设靠链提供 vendor）→ 4 悬空 + 4 缺 vendor 拒绝；循环继承/坏
JSON/空目录均拒绝；完整 bundle 通过（312 预设 / 222 实例化）。

## 静态库 ABI 提醒

引擎静态链接 libslic3r/CGAL/OCCT/Boost/OpenCV/TBB，任何 OrcaSlicer 变更
都需 regen StaticConfig + 重建（README "Updating when OrcaSlicer changes"）。

## 提交纪律

commit message 末尾**不加** Co-Authored-By / AI 署名行。
