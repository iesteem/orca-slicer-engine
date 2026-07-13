#pragma once

/**
 * @file ExitCodes.hpp
 * @brief Exit codes for the orca-slice-engine process.
 *
 * These codes are returned both by the standalone executable and by the
 * slic3r_slice() C API (as SLIC3R_ERR_* equivalents).  Values 0-7 are
 * shared between both interfaces; additional codes are API-specific.
 */

constexpr int EXIT_OK                   = 0;
constexpr int EXIT_INVALID_ARGS         = 1;
constexpr int EXIT_FILE_NOT_FOUND       = 2;
constexpr int EXIT_LOAD_ERROR           = 3;
constexpr int EXIT_SLICING_ERROR        = 4;
constexpr int EXIT_EXPORT_ERROR         = 5;
constexpr int EXIT_PREPROCESS_ERROR     = 6;  // 切片前校验错误（配置/输入/预设不合法）
constexpr int EXIT_POSTPROCESS_ERROR    = 7;  // 切片后致命错误（GCode已生成但内容不可用）
