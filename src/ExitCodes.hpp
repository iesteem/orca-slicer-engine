#pragma once

#include "slic3r_c_api.h" // SLIC3R_* status codes — single source for EXIT_*

// Exit codes — C++ aliases of the SLIC3R_* codes defined in slic3r_c_api.h
// (the C ABI header is the single source of truth; slic3r_slice() returns
// these values untranslated, so the two families can never drift).
inline constexpr int EXIT_OK = SLIC3R_OK;
inline constexpr int EXIT_INVALID_ARGS = SLIC3R_ERR_ARGS;
inline constexpr int EXIT_FILE_NOT_FOUND = SLIC3R_ERR_FILE_NOT_FOUND;
inline constexpr int EXIT_LOAD_ERROR = SLIC3R_ERR_LOAD;
inline constexpr int EXIT_SLICING_ERROR = SLIC3R_ERR_SLICING;
inline constexpr int EXIT_EXPORT_ERROR = SLIC3R_ERR_EXPORT;
inline constexpr int EXIT_PREPROCESS_ERROR = SLIC3R_ERR_PREPROCESS; // pre-slicing validation failure (config/input/presets invalid)
inline constexpr int EXIT_POSTPROCESS_ERROR = SLIC3R_ERR_POSTPROCESS; // post-slicing fatal error (GCode generated but unusable)
inline constexpr int EXIT_INTERNAL = SLIC3R_ERR_INTERNAL; // C++ exception crossing the API boundary
