# P-05: all-or-nothing 打包（宪法级设计决策）

**状态：** 用户确认的设计决策（2026-07-28），**不要当 bug 修**

全盘模式下 `package_output()`（SliceEngine.cpp:224 附近）仅在
`has_output && !m_any_error` 时打包 gcode.3mf。任一盘失败 → 10/11 盘成功
也拿不到 gcode.3mf（只有 out.json + out.log），退出码仍为
EXIT_SLICING_ERROR(4)。

若将来要改"部分成功也打包"：需人类批准 + 改打包条件（去 `!m_any_error`），
保持退出码 4 标识有失败盘。

## 附带契约

- 日志 `Filtered model: ... on plate N` 是 **1-based 用户盘号**；
  `Starting slicing for plate M` 是 **0-based 内部索引**；JSON 用 1-based。
