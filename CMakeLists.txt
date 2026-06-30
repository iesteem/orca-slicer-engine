cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 1.0.0 LANGUAGES C CXX)

# ==================================================================
# Two build modes:
#   Consumer mode (default):  build main.c against prebuilt slic3r.dll
#                             cmake -S . -B build -DCMAKE_PREFIX_PATH=<dll_dir>
#
#   DLL mode:                 build slic3r.dll from engine source + libslic3r SDK
#                             cmake -S . -B build -DBUILD_SLIC3R_DLL=ON
#                             -DCMAKE_PREFIX_PATH=<OrcaSlicer SDK dir>
# ==================================================================

option(BUILD_SLIC3R_DLL "Build slic3r.dll from engine source (requires libslic3r SDK)" OFF)
option(ORCA_STATIC "Build a single static executable linking libslic3r.a + all deps (Linux)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================================================================
# Static single-binary mode (Linux):
#   Compile the engine C++ sources + main.c straight into ONE executable
#   that statically links libslic3r.a and every dependency .a — no .so at all.
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
        src/nanosvg.cpp
        src/PresetRollback.cpp
    )

    # Drop include dirs that no longer exist (e.g. build-time symlinks src/eigen)
    set(_inc_dirs "${CMAKE_CURRENT_SOURCE_DIR}/src")
    foreach(_d IN LISTS ORCA_STATIC_INCLUDES)
        if(EXISTS "${_d}")
            list(APPEND _inc_dirs "${_d}")
        endif()
    endforeach()
    target_include_directories(orca-slice-engine PRIVATE ${_inc_dirs})

    target_compile_definitions(orca-slice-engine PRIVATE ${ORCA_STATIC_DEFINES})

    # Match libslic3r's translation-unit flags so our sources agree with its headers.
    target_compile_options(orca-slice-engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fext-numeric-literals;-fsigned-char>)
    set_target_properties(orca-slice-engine PROPERTIES
        CXX_STANDARD 17 CXX_EXTENSIONS ON           # -std=gnu++17 (libslic3r uses GNU exts)
        POSITION_INDEPENDENT_CODE ON)

    target_link_libraries(orca-slice-engine PRIVATE ${ORCA_STATIC_LIBS})

    message(STATUS "ORCA_STATIC enabled — single static executable")
endif()

# === Target 1: slic3r.dll (opt-in, needs OrcaSlicer SDK) ===
if(NOT ORCA_STATIC AND BUILD_SLIC3R_DLL)
    find_package(libslic3r REQUIRED)
    find_package(CGAL REQUIRED)
    find_package(Boost REQUIRED COMPONENTS filesystem log log_setup nowide)

    add_library(slic3r SHARED
        src/slic3r_c_api.h
        src/slic3r_c_api.cpp
        src/SliceEngine.cpp
        src/JsonReport.cpp
        src/GeometryCheck.cpp
        src/Utils.cpp
        src/nanosvg.cpp
        src/PresetRollback.cpp
    )

    target_include_directories(slic3r PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LIBSLIC3R_INCLUDE_DIRS}
        ${CGAL_INCLUDE_DIRS}
    )

    target_compile_definitions(slic3r PRIVATE
        SLIC3R_DLL_EXPORTS USE_TBB TBB_USE_CAPTURED_EXCEPTION=0
        BOOST_ALL_NO_LIB _USE_MATH_DEFINES
    )

    if(MSVC)
        target_compile_options(slic3r PRIVATE "/UBOOST_ALL_DYN_LINK")
        target_link_options(slic3r PRIVATE "/LIBPATH:${CMAKE_PREFIX_PATH}/lib")
    endif()

    target_link_libraries(slic3r PRIVATE
        libslic3r::libslic3r CGAL::CGAL
        Boost::filesystem Boost::log Boost::log_setup Boost::nowide
        TBB::tbb TBB::tbbmalloc OpenSSL::SSL OpenSSL::Crypto
        # OCCT
        TKG2d TKG3d TKMath TKernel TKGeomBase TKGeomAlgo
        TKBRep TKTopAlgo TKPrim TKHLR TKShHealing TKBO TKBool
        TKMesh TKXSBase TKSTEP TKSTEPAttr TKSTEPBase TKSTEP209
        TKIGES TKSTL TKXml TKXmlL TKXmlXCAF
        TKLCAF TKCAF TKBinL TKXCAF TKBin TKBinXCAF TKVCAF
        TKCDF TKV3d TKService TKRWMesh TKVRML TKXDEIGES TKXDESTEP
    )

    if(WIN32)
        target_link_libraries(slic3r PRIVATE Psapi.lib bcrypt.lib)
    endif()

    message(STATUS "slic3r.dll target enabled")
endif()

# === Target 2: orca-slice-engine.exe (consumer; skipped in ORCA_STATIC mode) ===

if(NOT ORCA_STATIC)
if(BUILD_SLIC3R_DLL)
    # DLL is being built locally — link against it
    add_executable(orca-slice-engine src/main.c)
    target_include_directories(orca-slice-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(orca-slice-engine PRIVATE slic3r)
else()
    # Consumer mode — find prebuilt DLL
    find_library(SLIC3R_LIBRARY slic3r
        HINTS "${CMAKE_PREFIX_PATH}/lib" "${CMAKE_PREFIX_PATH}/bin" "${CMAKE_PREFIX_PATH}"
        REQUIRED)
    message(STATUS "Found slic3r: ${SLIC3R_LIBRARY}")

    add_executable(orca-slice-engine src/main.c)
    target_include_directories(orca-slice-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(orca-slice-engine PRIVATE ${SLIC3R_LIBRARY})
endif()
endif()

message(STATUS "orca-slice-engine configured (C API consumer)")
