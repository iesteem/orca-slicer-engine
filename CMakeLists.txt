cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 1.0.0 LANGUAGES C CXX)

# ==================================================================
# Two build modes:
#   Consumer mode (default):  build main.c against prebuilt slic3r.dll
#                             cmake -S . -B build -DCMAKE_PREFIX_PATH=<dll_dir>
#
#   DLL mode:                 build slic3r.dll from engine source + libslic3r SDK
#                             cmake -S . -B build -DBUILD_SLIC3R_DLL=ON
#                             -DLIBSLIC3R_DIR=<OrcaSlicer source dir>
#                             -DCMAKE_PREFIX_PATH=<OrcaSlicer deps install dir>
# ==================================================================

option(BUILD_SLIC3R_DLL "Build slic3r.dll from engine source (requires libslic3r SDK)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Target 1: slic3r.dll (opt-in, needs OrcaSlicer SDK) ===
if(BUILD_SLIC3R_DLL)

    # --- libslic3r discovery ---
    # libslic3r doesn't provide a CMake config file, so we discover it manually.
    # LIBSLIC3R_DIR points to the OrcaSlicer source root (e.g. C:/code/iesteem_build_OrcaSlicer).
    # The static library is expected at <LIBSLIC3R_DIR>/build/src/libslic3r/Release/libslic3r.lib
    # (or the equivalent under CMAKE_BUILD_TYPE).
    set(LIBSLIC3R_DIR "" CACHE PATH "OrcaSlicer source directory containing src/libslic3r/")
    if(NOT LIBSLIC3R_DIR)
        message(FATAL_ERROR "LIBSLIC3R_DIR is required for DLL mode. "
            "Pass -DLIBSLIC3R_DIR=<path/to/OrcaSlicer>")
    endif()

    # Include directories
    set(LIBSLIC3R_INCLUDE_DIRS
        ${LIBSLIC3R_DIR}/src                                    # libslic3r headers
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include  # CGAL, Boost, OCCT, etc.
    )

    # Library directory for OCCT
    set(OCCT_LIB_DIR ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/lib/occt)

    # libslic3r static library (Release config; use Debug path for Debug builds)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(LIBSLIC3R_LIBRARY ${LIBSLIC3R_DIR}/build/src/libslic3r/Debug/libslic3r.lib)
    else()
        set(LIBSLIC3R_LIBRARY ${LIBSLIC3R_DIR}/build/src/libslic3r/Release/libslic3r.lib)
    endif()

    if(NOT EXISTS "${LIBSLIC3R_LIBRARY}")
        message(FATAL_ERROR "libslic3r.lib not found at ${LIBSLIC3R_LIBRARY}. "
            "Build OrcaSlicer first (${CMAKE_BUILD_TYPE} config).")
    endif()
    message(STATUS "Using libslic3r: ${LIBSLIC3R_LIBRARY}")

    add_library(slic3r SHARED
        src/slic3r_c_api.h
        src/slic3r_c_api.cpp
        src/SliceEngine.cpp
        src/JsonReport.cpp
        src/GeometryCheck.cpp
        src/Utils.cpp
        src/nanosvg.cpp
    )

    target_include_directories(slic3r PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LIBSLIC3R_INCLUDE_DIRS}
    )

    target_compile_definitions(slic3r PRIVATE
        SLIC3R_DLL_EXPORTS USE_TBB TBB_USE_CAPTURED_EXCEPTION=0
        BOOST_ALL_NO_LIB _USE_MATH_DEFINES
    )

    if(MSVC)
        target_compile_options(slic3r PRIVATE "/UBOOST_ALL_DYN_LINK")
        target_link_options(slic3r PRIVATE "/LIBPATH:${CMAKE_PREFIX_PATH}/lib")
        target_link_options(slic3r PRIVATE "/LIBPATH:${OCCT_LIB_DIR}")
    endif()

    target_link_libraries(slic3r PRIVATE
        ${LIBSLIC3R_LIBRARY}                       # libslic3r (static)
        # CGAL and Boost via find_package (found in CMAKE_PREFIX_PATH)
    )

    # ---- CGAL ----
    find_package(CGAL REQUIRED)
    target_include_directories(slic3r PRIVATE ${CGAL_INCLUDE_DIRS})
    target_link_libraries(slic3r PRIVATE CGAL::CGAL)

    # ---- Boost ----
    find_package(Boost REQUIRED COMPONENTS filesystem log log_setup nowide)
    target_link_libraries(slic3r PRIVATE
        Boost::filesystem Boost::log Boost::log_setup Boost::nowide
    )

    # ---- TBB ----
    find_package(TBB REQUIRED)
    target_link_libraries(slic3r PRIVATE TBB::tbb TBB::tbbmalloc)

    # ---- OpenSSL ----
    find_package(OpenSSL REQUIRED)
    target_link_libraries(slic3r PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    # ---- OCCT (manually linked — no CMake config provided) ----
    set(OCCT_LIBS
        TKG2d TKG3d TKMath TKernel TKGeomBase TKGeomAlgo
        TKBRep TKTopAlgo TKPrim TKHLR TKShHealing TKBO TKBool
        TKMesh TKXSBase TKSTEP TKSTEPAttr TKSTEPBase TKSTEP209
        TKIGES TKSTL TKXml TKXmlL TKXmlXCAF
        TKLCAF TKCAF TKBinL TKXCAF TKBin TKBinXCAF TKVCAF
        TKCDF TKV3d TKService TKRWMesh TKVRML TKXDEIGES TKXDESTEP
    )
    foreach(occt_lib ${OCCT_LIBS})
        target_link_libraries(slic3r PRIVATE ${OCCT_LIB_DIR}/${occt_lib}.lib)
    endforeach()

    # ---- GMP / MPFR (math) ----
    target_link_libraries(slic3r PRIVATE
        ${CMAKE_PREFIX_PATH}/lib/libgmp-10.lib
        ${CMAKE_PREFIX_PATH}/lib/libmpfr-4.lib
    )

    if(WIN32)
        target_link_libraries(slic3r PRIVATE Psapi.lib bcrypt.lib)
    endif()

    message(STATUS "slic3r.dll target enabled")
endif()

# === Target 2: orca-slice-engine.exe ===

if(BUILD_SLIC3R_DLL)
    # DLL is being built locally — use main.cpp (C++ entry point with full pipeline control)
    add_executable(orca-slice-engine src/main.cpp)
    target_include_directories(orca-slice-engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LIBSLIC3R_INCLUDE_DIRS}
    )
    target_link_libraries(orca-slice-engine PRIVATE slic3r)
    if(MSVC)
        target_link_options(orca-slice-engine PRIVATE "/LIBPATH:${CMAKE_PREFIX_PATH}/lib")
        target_link_options(orca-slice-engine PRIVATE "/LIBPATH:${OCCT_LIB_DIR}")
    endif()
    message(STATUS "orca-slice-engine configured (C++ entry point)")
else()
    # Consumer mode — find prebuilt DLL, use main.c (pure C consumer)
    find_library(SLIC3R_LIBRARY slic3r
        HINTS "${CMAKE_PREFIX_PATH}/lib" "${CMAKE_PREFIX_PATH}/bin" "${CMAKE_PREFIX_PATH}"
        REQUIRED)
    message(STATUS "Found slic3r: ${SLIC3R_LIBRARY}")

    add_executable(orca-slice-engine src/main.c)
    target_include_directories(orca-slice-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(orca-slice-engine PRIVATE ${SLIC3R_LIBRARY})
    message(STATUS "orca-slice-engine configured (C API consumer)")
endif()
