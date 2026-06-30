# 清理计划:统一为 ORCA_STANDALONE 静态单模式

> 状态:**待执行(尚未动手)**
> 记录日期:2026-06-30
> 决策:服务器「每个模型起一个引擎进程、跑完退出」场景下,**只保留 `ORCA_STANDALONE=ON` 静态单体**,
> 配合 mold/lld 加速链接;**全面清除动态链接库(libslic3r.so / Windows slic3r.dll)那一整套**。

---

## 一、为什么这么做(决策依据)

1. **内存:静态 vs 动态打平。** 服务器只 spawn 同一个二进制,静态单体的 text 段经内核 page cache 跨进程共享,和动态库省内存效果一样。动态库省内存的前提是「多个不同可执行文件共用一份 .so」——本项目不是该场景。
2. **部署正确性:静态明显赢(决定性理由)。** 动态模式带来 `libslic3r.so.2` 软链 + RPATH(`$ORIGIN/../lib`)+ 运行时加载,是「加载到错误版本 .so」这类 bug 的温床。静态单文件部署,无依赖、无软链。
3. **可能直接消除已知崩溃。** 记忆 [[v02-01-18-heap-corruption-crash]](`~GCodeConfig()` 析构 `vector<string>` 时 `free(): invalid pointer`,v17 正常 v18 崩)高度疑似跨 .so 边界 + allocator 混用(.so 内链了 `libtbbmalloc.a`)/ 版本错配。静态单体统一 allocator、无 .so 边界,从根上消除这一整类问题。**NOT VERIFIED**:尚未实测确认,但静态能排除该变量。
4. **启动延迟:静态略快(次要)。** 无 `ld.so` 每次 exec 的重定位 + 符号绑定;因进程立即退出,动态库那笔解析开销永远摊销不掉。但切片本身耗时数秒~数分钟,这点差异通常可忽略。
5. **日常开发编译不会变慢。** 增量编译只重编改动的 TU,两模式一致;慢的是链接几十 M 的 `.a`,而静态/动态都要链同样一批,基本持平。动态唯一占便宜的场景是「只改 `main.c`/C-API 瘦壳」,但这种改动很少。

---

## 二、现状全貌:动态库体系横跨 6 层

| 层 | 动态库专属内容 | 文件/行号 |
|---|---|---|
| CMake | `BUILD_SLIC3R_DLL` 整块(Windows DLL + Linux SO) | `CMakeLists.txt` 206–333(Win)、336–523(Linux SO) |
| CMake | Consumer 默认块(链预编译 .so) | `CMakeLists.txt` 532–541 |
| 源码 | C-API 层(仅动态模式用;静态走 `main.cpp`) | `src/main.c`、`src/slic3r_c_api.cpp`、`src/slic3r_c_api.h` |
| Git 产物 | 跟踪的 .so(注意跟踪的是 **v17**,工作区在删它) | `package/lib/libslic3r.so`、`.so.02.01.17`、`.so.2` |
| 脚本 | 构建/打包/SDK | `scripts/build.sh`(调 build-consumer)、`scripts/package_consumer.sh`(.so 软链逻辑)、`scripts/package_sdk.sh`(Windows DLL SDK)、`scripts/build_consumer_windows.ps1` |
| 构建目录 | 本地物,已 gitignore | `build-dll`(244M)、`build-consumer`(368K) |
| 文档 | 多处引用 | `README.md`、`修改记录.txt` |

**保留侧(静态单模式实际用到的):**
- CMake `ORCA_STANDALONE` 块(`CMakeLists.txt` 27–198)
- 源码 `src/main.cpp` + 所有引擎 `.cpp/.hpp`(EngineCLI、SliceEngine、PresetManager、PresetRollback、PlateProcessor、StatisticsBuilder、JsonReport、GeometryCheck、Utils、nanosvg)
- `package/bin/`、`package/resources/`

---

## 三、清理动作清单(待执行)

### CMake(`CMakeLists.txt`)
- [ ] 删除 `BUILD_SLIC3R_DLL` 整块(Windows DLL 段 + Linux SO 段)
- [ ] 删除末尾 Consumer 默认块(532–541)
- [ ] 把 `ORCA_STANDALONE` 设为默认行为(无需再传 `-DORCA_STANDALONE=ON`),或保留 option 但默认 ON
- [ ] 移除 `option(BUILD_SLIC3R_DLL ...)` 声明
- [ ] (可选)预留链接器选项位:`-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"`,见第四节

### 源码(`src/`)
- [ ] 删除 `src/main.c`
- [ ] 删除 `src/slic3r_c_api.cpp`
- [ ] 删除 `src/slic3r_c_api.h`

### Git 跟踪产物
- [ ] `git rm package/lib/libslic3r.so package/lib/libslic3r.so.02.01.17 package/lib/libslic3r.so.2`
- [ ] 决定 `package/lib/` 目录去留(静态产物为单 exe,理论上不再需要 lib/)

### 脚本(`scripts/`)
- [ ] 重写 `scripts/build.sh` 为 standalone 单模式,**编译命令带 `-j 2`**(见记忆 [[build-parallelism-j2]])
- [ ] 重写 `scripts/package_consumer.sh`:去掉 .so 拷贝 + 软链逻辑,只打包单 exe + resources
- [ ] 删除 `scripts/package_sdk.sh`(纯 Windows DLL SDK 打包)
- [ ] 删除 `scripts/build_consumer_windows.ps1`(纯 Windows consumer)

### 文档与记忆
- [ ] `README.md`:更新为单一静态模式构建说明
- [ ] `修改记录.txt`:追加本次架构收敛记录
- [ ] 新增记忆(project/feedback):「本项目只用 ORCA_STANDALONE 静态单模式」
- [ ] 更新记忆 [[v02-01-18-heap-corruption-crash]]:标注动态 .so 体系已废弃,该崩溃路径随之消除

---

## 四、待你拍板的决策点(执行前必须确认)

### 决策点 1:清理深度
- **A. 全删,只留静态** — 最干净、单一事实来源,但永久放弃「构建 libslic3r.so」与「Windows slic3r.dll」两条路径(git 历史可找回)。
- **B. 删 Linux 动态,留 Windows DLL** — 保留 `BUILD_SLIC3R_DLL` 的 Windows 段 + `slic3r_c_api`,将来仍能在 Windows 出 DLL。
- **C. 先只改默认 + 加 DEPRECATED 注释** — 最保守、完全可逆,留作过渡。
- ❓ 待确认:Windows `slic3r.dll` 当前是否仍有客户/场景在用?是否有「引擎当库嵌入别的程序」的未来计划?

### 决策点 2:链接器(mold 现在装不了)
- 现状:**本机无 mold/lld**,且 **gcc 11.4 不支持 `-fuse-ld=mold`**(需 gcc≥12 或任意 clang)。
- **A. 装 lld**,用 `-fuse-ld=lld`(gcc11 支持),立即可用,需 `sudo apt install lld`。
- **B. 装 mold**,gcc11 走「替换默认 ld」方式接线,需 sudo。
- **C. 暂不动链接器**,本轮只做清理,保持 GNU ld,mold/lld 留作独立一步。
- 建议:链接器与清理**解耦**,先清理,再单独处理加速。

### 决策点 3:.so 产物处理
- **A. `git rm` 全部 .so**,并从打包流程移除 `package/lib/`,彻底消除版本错配源头。
- **B. `git rm` .so 文件但保留空 `package/lib/`**(加 .gitkeep),以防打包脚本结构依赖。

---

## 五、注意事项

- 编译验证:清理后用 `ORCA_STANDALONE=ON` 全量重建一次,**务必带 `-j 2`**(见 [[build-parallelism-j2]],否则本机卡死)。
- 行为验证(把崩溃假设坐实):用之前 v18 崩溃的模型跑静态版,确认 `free(): invalid pointer` 是否消失。
- 当前 `git status` 工作区已有 .so 增删改动(删 v17、加 02.02.01、改 libslic3r.so.2),执行清理前先与这些未提交改动协调,避免冲突。
