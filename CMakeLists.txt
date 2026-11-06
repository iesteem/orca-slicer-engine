cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 1.0.0 LANGUAGES C CXX)

# ==================================================================
# Build mode (single, supported mode):
#   ORCA_STATIC:  compile main.c (C CLI) + the engine C++ sources into ONE
#                 static executable that statically links libslic3r.a and all
#                 dependencies (Linux only). See the block below for usage.
# ==================================================================

option(ORCA_STATIC "Build a single static executable linking libslic3r.a + all deps (only supported mode, Linux)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================================================================
# Canonical engine version — SINGLE SOURCE OF TRUTH.
# The binary, the unit/integration tests, and scripts/package_linux.sh
# all derive the version string from this value (no duplication elsewhere).
# ==================================================================
set(ENGINE_VERSION "2.2.1")

# ==================================================================
# Warning flags for OWN code (B6 P2, 2026-11-06). External headers (fork
# libslic3r, boost, ...) are consumed through SYSTEM include dirs (see the
# targets below) so their ~1.4k probe-measured warnings never mix in; what
# remains under these flags is ours and must be zero.
# P4 (-Werror) is deliberately NOT enabled yet: it needs a clean probe run
# on every future compiler before it can act as a gate.
# (Defined before first use — the engine target below calls it.)
# ==================================================================
function(orca_target_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wformat=2
        $<$<COMPILE_LANGUAGE:CXX>:-Wshadow;-Wnon-virtual-dtor;-Woverloaded-virtual>)
endfunction()

# ==================================================================
# Static single-binary mode (Linux):
#   Compile the engine C++ sources + main.c straight into ONE executable
#   that statically links libslic3r.a and every bundled dependency .a. Only the
#   standard system runtime stays dynamic (libstdc++/libc/... per readelf -d).
#
#   The authoritative library list / include dirs / defines are extracted from
#   an existing OrcaSlicer build by scripts/gen_static_libs.sh into
#   cmake/StaticConfig.cmake. Regenerate that file whenever OrcaSlicer is rebuilt.
#
#   Usage:
#     ORCA_ROOT=/path/to/OrcaSlicer scripts/gen_static_libs.sh
#     cmake -S . -B build-static -DORCA_STATIC=ON
#     cmake --build build-static -j 2
# ==================================================================
if(ORCA_STATIC)
    set(_static_cfg "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StaticConfig.cmake")
    if(NOT EXISTS "${_static_cfg}")
        message(FATAL_ERROR
            "ORCA_STATIC=ON but ${_static_cfg} is missing.\n"
            "Run: ORCA_ROOT=<OrcaSlicer path> scripts/gen_static_libs.sh")
    endif()
    include("${_static_cfg}")  # defines ORCA_STATIC_LIBS / ORCA_STATIC_INCLUDES / ORCA_STATIC_DEFINES

    add_executable(orca-slice-engine
        src/main.c
        src/slic3r_c_api.cpp
        src/SliceEngine.cpp
        src/JsonReport.cpp
        src/GeometryCheck.cpp
        src/Utils.cpp
        src/PresetRollback.cpp
        # src/nanosvg.cpp removed: Orca base 2.4.1 embeds NANOSVG_IMPLEMENTATION
        # in libslic3r/Format/svg.cpp, so the headless shim now duplicate-defines
        # nsvg* symbols at static link (multiple definition of nsvg__colors).
    )

    # Drop include dirs that no longer exist (e.g. build-time symlinks src/eigen)
    set(_ext_inc_dirs "")
    foreach(_d IN LISTS ORCA_STATIC_INCLUDES)
        if(EXISTS "${_d}")
            list(APPEND _ext_inc_dirs "${_d}")
        endif()
    endforeach()
    # Own src/ stays non-SYSTEM; every dependency header dir is SYSTEM so
    # warnings in fork/boost headers stay out of our build output (B6 P1).
    target_include_directories(orca-slice-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(orca-slice-engine SYSTEM PRIVATE ${_ext_inc_dirs})

    target_compile_definitions(orca-slice-engine PRIVATE
        ${ORCA_STATIC_DEFINES}
        ENGINE_VERSION="${ENGINE_VERSION}"
    )

    # Match libslic3r's translation-unit flags so our sources agree with its headers.
    target_compile_options(orca-slice-engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fext-numeric-literals;-fsigned-char>)
    orca_target_warnings(orca-slice-engine)
    set_target_properties(orca-slice-engine PROPERTIES
        CXX_STANDARD 17 CXX_EXTENSIONS ON           # -std=gnu++17 (libslic3r uses GNU exts)
        POSITION_INDEPENDENT_CODE ON)

    target_link_libraries(orca-slice-engine PRIVATE ${ORCA_STATIC_LIBS})

    # The prebuilt archives carry LTO; at link time GCC then analyzes TBB's
    # own compiled-in types and flags ODR violations inside fork/dependency
    # code (-Wodr from dep_TBB Customize.h). SYSTEM include dirs cannot
    # suppress link-time diagnostics, so silence the flag here — it is
    # fork-side and not actionable from this repo (B6 P2).
    target_link_options(orca-slice-engine PRIVATE -Wno-odr)

    # Extract the boost static archives from ORCA_STATIC_LIBS so the unit-test
    # target (which does NOT link the full engine) can reuse the same private
    # boost build without hard-coding absolute paths. boost.log pulls in
    # boost.thread -> chrono -> atomic transitively, so grab the whole boost
    # set rather than a minimal subset (link order within one list is handled
    # by the linker's rescan of static archives).
    set(ORCA_STATIC_BOOST_LIBS "")
    foreach(_lib IN LISTS ORCA_STATIC_LIBS)
        if(_lib MATCHES "libboost_.*\\.a$")
            list(APPEND ORCA_STATIC_BOOST_LIBS "${_lib}")
        endif()
    endforeach()

    message(STATUS "ORCA_STATIC enabled — single static executable")
endif()

message(STATUS "orca-slice-engine configured (single static binary)")

# ==================================================================
# Unit tests — independent of the engine targets above. Only exercises
# the utility/inline-factory code in src/, links boost (not libslic3r).
# Build & run:
#   cmake --build build-static --target engine-tests
#   ctest --test-dir build-static -R engine-tests --output-on-failure
# ==================================================================
enable_testing()
add_subdirectory(tests)
