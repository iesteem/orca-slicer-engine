# P-20: restore_brim_width_echo 回显还原的 I/O 失败语义与劫持面加固

**状态：** 已修复（2026-09-20，stage 2.1；基于 f291619 对抗审查结论，变更在
worktree 未单独提交时记录，见"遗留问题"）

**上游关联：** P-11（brim-auto-divergence.md）stage 2。f291619 修复了
"回显说谎"（brim_width 写回 0），本节记录对其修复实现本身的循环对抗
审查与加固。

**审查方法：** harness 协议（Part B）三视角×两轮循环对抗
（Round 0 本人取证 → Round 1 三独立 agent 并行对抗：I/O 失败语义 /
配置流全消费面 / 测试与文档声明 → Round 2 逐条怀疑式验证收敛）。
两 agent 独立命中同一 P1（收敛信号）；Round 2 证伪 4 项假设。

---

## 一、Bug 描述

f291619 引入的 `restore_brim_width_echo()`（export 后把 gcode 中
`; brim_width = ` 回显行改写回 m_config 值）在**正常路径**行为正确
（审查实证：print 输入逐位相同、两个回写面恢复、43 断言全绿），但实现
带一个正确性缺陷和三个健壮性/安全性缺陷：

| # | 级别 | 缺陷 | 现象 |
|---|------|------|------|
| F1 | **P1 (C)** | rename 前不校验流状态 | 磁盘满/读错误时，**截断的 tmp 静默 rename 覆盖完好的 gcode**，且日志打 "restored" INFO；截断文件随后被打包进 gcode.3mf 交付（md5 自洽，无告警） |
| F2 | P2 (C) | restore 位于 export_gcode 的 try 内 | 函数内任何异常（如不受信 machine gcode 超长行触发 bad_alloc）被 catch 判成 `GCODE_EXPORT_ERROR` → 经 all-or-nothing 打包放大：**cosmetic 步失败使整单（如 10/11 盘成功）无产出**，错误文案误导 |
| F3 | P2 (N) | tmp 文件无 RAII 属主、未注册 m_temp_files | 异常逃逸/进程被杀（300MB 逐行拷贝期间的超时强杀）泄漏全尺寸 `.brimecho.tmp`；single_plate GCODE 模式下泄漏落在**用户可见的输出目录** |
| F4 | P2 (C) | 首匹配无锚定 | 非 BBL 打印机（Snapmaker 全系）真回显块在**文件尾**（GCode.cpp:3100），用户自定义 gcode 在其前——不受信 3MF 的 machine gcode 里一条 `; brim_width = X` 注释会**劫持改写**（用户行被改、真回显保持 0），本修复对该输入静默复发 |

测试/文档配套缺口（P2）：P-11 验收点"含 3mf 内嵌 config"零自动化覆盖
（F5）；重写的 preset invariant 沿用软守卫 `if (bt)`，与刚修掉的死代码
同机制，可再次静默空转（F6）。

## 二、根因分析

1. **F1**：`while (std::getline(in, line))` 在 badbit（读 I/O 错误）下与
   EOF 同样静默退出，循环无法区分；写侧 ENOSPC 因 ofstream 缓冲延迟到
   `close()` 的 flush 才置 failbit（默认 exceptions mask 下不抛）。旧代码
   在 close 与 rename 之间**没有任何状态检查**——"先写副本再提交"的
   strong-guarantee 骨架搭了，提交门没装。仓内范本：上游 GCode.cpp:1812
   -1816 自己就是 `flush() + is_error()` 才 rename。触发面真实：restore
   紧跟全量 gcode 写出、自己再写第二份全量副本，正是磁盘余量最紧时刻。
2. **F2**：exception-safety Item 6（weakest-link 组合）违例——best-effort
   的 cosmetic 步骤被组合进必须成功的 export 事务，其异常语义被调用方
   catch 升级为整盘失败。
3. **F3**：raii.md Item 1 违例——资源（tmp 磁盘文件）在创建点无属主，
   4 条退出路径中 2 条手工 remove，异常/崩溃路径全部漏删
   （Anti-Pattern 1 手工清理的教科书形态）。
4. **F4**：匹配谓词是全文件首条前缀命中。fork 的回显排版实证：BBL 打印
   机 config block 在文件头、非 BBL 在**尾部**，而 machine start/end
   gcode 由不受信 3MF 项目 config 逐字透传（GCode.cpp:2761/3061）且先于
   尾部块写出——云引擎处理不受信上传，构成可构造的劫持通道。
5. **F5/F6**：测试断言面止步于 gcode 文本；软守卫是 e76c1ab 引入死代码
   的同一机制（`option<T>` 类型不匹配返回 nullptr → if 静默跳过）。

## 三、修复方案（stage 2.1）

单函数内四层防御，全部失败路径降级为日志、原 gcode 不动：

1. **提交门（F1）**：`in.close(); out.close();` 之后、rename 之前
   `if (in.bad() || !out.good())` → error 日志 + 保留原文件返回。
   写失败在 close 的 flush 浮现、读失败置 badbit 早停循环，两者都被拦截。
2. **异常自含（F2）**：函数体整体 `try/catch(const std::exception&)`
   降级 warning——注释行改写失败不再可能把成功导出变失败。
3. **tmp RAII（F3）**：函数内局部 `TmpFileGuard`（path + committed 标志），
   析构用 `boost::filesystem::remove` 的 `error_code` 重载保 no-throw；
   rename 成功后置 `committed = true`。覆盖全部退出路径。
4. **双锚定（F4）**：只在两个条件同时满足时替换——
   (a) 行位于 `; CONFIG_BLOCK_START/END` 区间内（状态机跟踪，BBL 头部块
   与非 BBL 尾部块同构覆盖）；(b) 旧行值恰为 `"0"`（清零切片副本的
   确定产物）。
   **自我对抗记录**：仅 (a) 可被伪造的 `; CONFIG_BLOCK_START` 行欺骗
   （攻击者在 machine gcode 里造块标记 + `; brim_width = 8`），补 (b)
   后劫持面压缩到"逐字复制引擎自身回显行"——此时文件内出现双值行，
   攻击者本已控制该文件内容，残险可接受。
   副产品：若未来盘级/对象级覆盖使回显值 ≠ 0（当前不可达，见 P-11
   "已证伪假设"），值校验会跳过改写而非写错值，天然前向安全。
   附加：`m_config brim_width == 0` 时整体早退（零值回显本就正确，
   免全文件空转拷贝）。
5. **测试补强**：brim 回归用例新增输出 3mf `project_settings.config`
   断言（F5，第二回写面纳入 CI）；preset invariant 软守卫改 REQUIRE
   （F6，前置条件大声失败）。

## 四、修改内容

| 文件 | 内容 |
|------|------|
| `src/SliceEngine.cpp` | `restore_brim_width_echo` 重写（:3045 起）：早退 :3067、TmpFileGuard :3099、双锚定循环 :3117-3146、提交门 :3151、committed 提交 :3172、外层 catch 降级 |
| `src/SliceEngine.hpp` | 声明注释同步加固契约（锚定 / 提交门 / RAII / best-effort 永不阻断导出） |
| `tests/test_high_flow.cpp` | 新增 `extract_3mf_text()`（:72，python3 zipfile shell-out，同 extract_gcode 模式、预清理产物）；brim 用例补 3mf config 断言（:233）与临时文件清理 |
| `tests/test_preset_substitution.cpp` | invariant 前置条件改 `REQUIRE(bt)` / `REQUIRE(bt->value == btAutoBrim)`（:263） |
| `docs/precedents/brim-auto-divergence.md` | P-11 追加"Stage 2.1 加固"节；修正 `Print.cpp:2168 margin 门` 死代码引用（`#if 0` 内，实际消费点 Brim.cpp:61/207-212/917、PrintObject.cpp:698） |

输出 3mf config 断言格式依据：实测并行产物 `Metadata/project_settings.config`
为 **JSON**（`"brim_width": "5",`）——非 ini `key = value`，断言串不可想当然。

## 五、实际测试

| 检查 | 证据 | 结果 |
|------|------|------|
| 编译 | `make -j2 engine-integration`（构建前确认无并行 make） | exit 0；二进制 mtime(14:39) > 源码编辑(14:19)，改动确已编入 |
| 目标回归 | `./engine-integration "[brim],[preset]"` | **48 断言/5 用例全绿**，恰好 = 修复前 43 + 新增 5（REQUIRE×2 + helper 内 REQUIRE×2 + 3mf 断言×1）——新断言全部真执行；双锚定改写在真实产物上命中（gcode echo=5 出现于尾部 CONFIG_BLOCK） |
| 副作用检查 | `./engine-integration "[slice]"` | **171 断言/22 用例全绿**（覆盖全部成功路径的 restore 触发，含并行会话新加 empty-plate 用例） |
| 审查轮基线 | 同二进制修复前状态 `[brim],[preset]` | 43 断言/5 用例全绿（审查 Round 2 动态基线） |

## 六、遗留问题

1. **未做故障注入验证（NOT VERIFIED）**：F1/F2/F3 的失败路径
   （ENOSPC、bad_alloc、kill -9）为构造性论证 + 代码审查，未仿真。
   补法：dev 文件系统配额 / fault-injection 小 fixture。
2. **F4 劫持负路径无 hostile fixture**：machine gcode 含
   `; brim_width =` 行的 3MF 未造；当前为代码级论证。造一个小 fixture
   断言"注入行原样保留 + 尾部真回显被改写"即可闭环。
3. **P3 建议项未做**（低风险，留档）：
   - 改写逻辑（istream→ostream、prefix/anchor 参数化）抽纯函数，
     补 Small 单测（现仅 Large 整切覆盖，金字塔 70/20/10 偏离）；
   - `"; brim_width = 5\n"` 硬编码耦合 profile 数据（官方链现值 5，
     Snapmaker 改值时引擎正确测试假红）——可运行时读 bundle 值拼串；
   - invariant `CHECK(bw==ow)` 与 check_matches_official 冗余，且未来
     fixture 把 brim_width 放进 dts 会误红（宜按 dts 条件化）；
   - 同一 8 轴 fixture 被完整切片两次（高流量用例 + brim 用例，各
     ~3.5 分钟），可 SECTION 共享一次 engine.run()；
   - fixture 双用途（P-13 高流量 + P-11 brim）未记入 MANIFEST。
4. **提交待办**：stage 2.1 变更在 worktree 与并行会话的 empty-plate
   WIP（run()/process_plate/test_slice_e2e/exit-codes 等 hunk）混居，
   提交时需按 hunk 分离（本仓既有部分暂存流程）。
5. **既有隐患留档**（本会话发现、未改）：`option<ConfigOptionEnum<T>>`
   对动态加载的 enum 键是跨兄弟层级 static_cast 的布局巧合
   （def 工厂恒造 ConfigOptionEnumGeneric，Config.cpp:287；类型检查
   `coEnum==coEnum` 恒过）——旧代码同惯用法已实证可用，若未来重构建议
   改 `ConfigOptionEnumGeneric`/`getInt()` 比较。

## 附：审查中证伪的假设（防复发）

| 假设 | 证伪证据 |
|------|----------|
| ostringstream 还原格式与回显序列化分歧 | `ConfigOptionFloat::serialize()` 本身就是 `ostringstream << value`（Config.hpp:578-583），逐字节同构 |
| pd->config 盘级 brim 覆盖导致回显错配 | bbs_3mf 导入器 plate config 是固定键集 if/else 链，brim 键进不来 |
| 无尾换行 +1 字节破坏逐行一致 | fork GCode.cpp:3115 无条件写尾 `\n`，不可达 |
| CRLF/NUL/空文件/权限位/rename 语义问题 | 逐项核查 PASS（binary 模式 + LF-only + 同目录同模式 0666&~umask） |

f291619 commit message 声明核查结论：**成立**（print 输入逐位相同、
两回写面恢复、死代码判断正确、"red by construction" 构造性成立），
唯 "rewrite the single line" 的唯一性无强制（即 F4，本次已修）。
