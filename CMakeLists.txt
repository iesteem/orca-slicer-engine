cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 02.02.01 LANGUAGES C CXX)
option(BUILD_SLIC3R_DLL "Build slic3r shared library from engine source (DLL on Windows, SO on Linux). Requires libslic3r SDK." OFF)
option(ORCA_STANDALONE "Linux standalone mode: statically link against OrcaSlicer build tree" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warning flags (coding-standards §2.3)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

# ==================================================================
# Linux standalone mode (opt-in): statically link against OrcaSlicer
# build tree (liblibslic3r.a + deps).
#
#   cmake -S . -B build -DORCA_STANDALONE=ON \
#       -DORCASLICER_BUILD_DIR=<OrcaSlicer build-engine dir> \
#       -DORCASLICER_SOURCE_DIR=<OrcaSlicer source dir>
# ==================================================================

if(ORCA_STANDALONE)
    set(ORCASLICER_BUILD_DIR "" CACHE PATH "OrcaSlicer build-engine directory")
    set(ORCASLICER_SOURCE_DIR "" CACHE PATH "OrcaSlicer source directory")

    if(NOT ORCASLICER_BUILD_DIR OR NOT ORCASLICER_SOURCE_DIR)
        message(FATAL_ERROR
            "Linux build requires:\n"
            "  -DORCASLICER_BUILD_DIR=<path/to/OrcaSlicer/build-engine>\n"
            "  -DORCASLICER_SOURCE_DIR=<path/to/OrcaSlicer>\n"
            "  Omit -DORCA_STANDALONE for default consumer mode (dynamic linking).")
    endif()

    set(DEPS_DIR "${ORCASLICER_SOURCE_DIR}/deps/build/destdir/usr/local")
    set(BUILD_DEPS_SRC "${ORCASLICER_BUILD_DIR}/deps_src")
    set(BUILD_LIB "${ORCASLICER_BUILD_DIR}/lib")

    message(STATUS "OrcaSlicer build dir: ${ORCASLICER_BUILD_DIR}")
    message(STATUS "OrcaSlicer source dir: ${ORCASLICER_SOURCE_DIR}")
    message(STATUS "Deps install dir: ${DEPS_DIR}")

    # === Shared library collections (de-duplicated across targets) ===

    set(ENGINE_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ORCASLICER_SOURCE_DIR}/src
        ${ORCASLICER_BUILD_DIR}/src
        ${ORCASLICER_SOURCE_DIR}/src/libslic3r
        ${ORCASLICER_BUILD_DIR}/src/libslic3r
    )

    set(ENGINE_SYSTEM_INCLUDE_DIRS
        ${DEPS_DIR}/include
        ${DEPS_DIR}/include/opencascade
        ${DEPS_DIR}/include/opencv4
        ${ORCASLICER_SOURCE_DIR}/deps_src/nlohmann
        ${ORCASLICER_SOURCE_DIR}/deps_src/nanosvg
        ${ORCASLICER_SOURCE_DIR}/deps_src/miniz
        ${ORCASLICER_SOURCE_DIR}/deps_src/eigen
        ${ORCASLICER_SOURCE_DIR}/deps_src/libigl
        ${ORCASLICER_SOURCE_DIR}/deps_src/admesh
        ${ORCASLICER_SOURCE_DIR}/deps_src/admesh/..
        ${ORCASLICER_SOURCE_DIR}/deps_src/libnest2d/include
    )

    set(DEPS_SRC_LIBS
        ${BUILD_DEPS_SRC}/libnest2d/liblibnest2d.a
        ${BUILD_DEPS_SRC}/miniz/libminiz_static.a
        ${BUILD_DEPS_SRC}/clipper/libclipper.a
        ${BUILD_DEPS_SRC}/glu-libtess/libglu-libtess.a
        ${BUILD_DEPS_SRC}/admesh/libadmesh.a
        ${BUILD_DEPS_SRC}/mcut/libmcut.a
        ${BUILD_DEPS_SRC}/qoi/libqoi.a
        ${BUILD_LIB}/libsemver.a
    )

    set(OCCT_LIBS
        ${DEPS_DIR}/lib/libTKXDESTEP.a
        ${DEPS_DIR}/lib/libTKSTEP.a
        ${DEPS_DIR}/lib/libTKSTEP209.a
        ${DEPS_DIR}/lib/libTKSTEPAttr.a
        ${DEPS_DIR}/lib/libTKSTEPBase.a
        ${DEPS_DIR}/lib/libTKXCAF.a
        ${DEPS_DIR}/lib/libTKXSBase.a
        ${DEPS_DIR}/lib/libTKVCAF.a
        ${DEPS_DIR}/lib/libTKCAF.a
        ${DEPS_DIR}/lib/libTKLCAF.a
        ${DEPS_DIR}/lib/libTKCDF.a
        ${DEPS_DIR}/lib/libTKV3d.a
        ${DEPS_DIR}/lib/libTKService.a
        ${DEPS_DIR}/lib/libTKMesh.a
        ${DEPS_DIR}/lib/libTKBO.a
        ${DEPS_DIR}/lib/libTKPrim.a
        ${DEPS_DIR}/lib/libTKHLR.a
        ${DEPS_DIR}/lib/libTKShHealing.a
        ${DEPS_DIR}/lib/libTKTopAlgo.a
        ${DEPS_DIR}/lib/libTKGeomAlgo.a
        ${DEPS_DIR}/lib/libTKBRep.a
        ${DEPS_DIR}/lib/libTKGeomBase.a
        ${DEPS_DIR}/lib/libTKG3d.a
        ${DEPS_DIR}/lib/libTKG2d.a
        ${DEPS_DIR}/lib/libTKMath.a
        ${DEPS_DIR}/lib/libTKernel.a
    )

    set(BOOST_LIBS
        ${DEPS_DIR}/lib/libboost_log_setup.a
        ${DEPS_DIR}/lib/libboost_log.a
        ${DEPS_DIR}/lib/libboost_filesystem.a
        ${DEPS_DIR}/lib/libboost_coroutine.a
        ${DEPS_DIR}/lib/libboost_context.a
        ${DEPS_DIR}/lib/libboost_serialization.a
        ${DEPS_DIR}/lib/libboost_locale.a
        ${DEPS_DIR}/lib/libboost_thread.a
        ${DEPS_DIR}/lib/libboost_chrono.a
        ${DEPS_DIR}/lib/libboost_atomic.a
        ${DEPS_DIR}/lib/libboost_date_time.a
        ${DEPS_DIR}/lib/libboost_exception.a
        ${DEPS_DIR}/lib/libboost_program_options.a
        ${DEPS_DIR}/lib/libboost_container.a
        ${DEPS_DIR}/lib/libboost_nowide.a
        ${DEPS_DIR}/lib/libboost_iostreams.a
        ${DEPS_DIR}/lib/libboost_random.a
    )

    set(MATH_LIBS
        ${DEPS_DIR}/lib/libgmpxx.a
        ${DEPS_DIR}/lib/libmpfr.a
        ${DEPS_DIR}/lib/libgmp.a
    )

    set(OTHER_DEPS_LIBS
        ${DEPS_DIR}/lib/liblibnoise_static.a
        ${DEPS_DIR}/lib/libpng.a
        ${DEPS_DIR}/lib/libqhullcpp.a
        ${DEPS_DIR}/lib/libqhullstatic_r.a
        ${DEPS_DIR}/lib/libjpeg.a
        ${DEPS_DIR}/lib/libfreetype.a
        ${DEPS_DIR}/lib/libopenvdb.a
        ${DEPS_DIR}/lib/libHalf-2_5.a
        ${DEPS_DIR}/lib/libblosc.a
        ${DEPS_DIR}/lib/libnlopt.a
    )

    # ---- Source files ----
    add_executable(orca-slice-engine
        src/main.cpp
        src/EngineCLI.cpp
        src/SliceEngine.cpp
        src/PresetManager.cpp
        src/PresetRollback.cpp
        src/PlateProcessor.cpp
        src/StatisticsBuilder.cpp
        src/JsonReport.cpp
        src/GeometryCheck.cpp
        src/Utils.cpp
        src/nanosvg.cpp
    )

    # ---- Include directories ----
    target_include_directories(orca-slice-engine PRIVATE ${ENGINE_INCLUDE_DIRS})
    target_include_directories(orca-slice-engine SYSTEM PRIVATE ${ENGINE_SYSTEM_INCLUDE_DIRS})

    # ---- Compile definitions ----
    target_compile_definitions(orca-slice-engine PRIVATE
        -DUSE_TBB
        -DTBB_USE_CAPTURED_EXCEPTION=0
        -DBOOST_ALL_NO_LIB
        "CLOUD_SLICER_ENGINE_VERSION=\"v02.02.01\""
    )

    # ---- Link all static libraries ----
    target_link_libraries(orca-slice-engine PRIVATE
        ${ORCASLICER_BUILD_DIR}/src/libslic3r/liblibslic3r.a
        ${ORCASLICER_BUILD_DIR}/src/libslic3r/liblibslic3r_cgal.a
        ${DEPS_DIR}/lib/libtbb.a
        ${DEPS_DIR}/lib/libtbbmalloc.a
        ${DEPS_DIR}/lib/libssl.a
        ${DEPS_DIR}/lib/libcrypto.a
        ${CMAKE_DL_LIBS}
        ${DEPS_SRC_LIBS}
        ${DEPS_DIR}/lib/libopencv_world.a
        ${DEPS_DIR}/lib/opencv4/3rdparty/liblibjpeg-turbo.a
        ${DEPS_DIR}/lib/opencv4/3rdparty/liblibpng.a
        ${OCCT_LIBS}
        ${BOOST_LIBS}
        ${MATH_LIBS}
        ${OTHER_DEPS_LIBS}
        pthread rt m fontconfig expat z tiff lzma
    )

    message(STATUS "orca-slice-engine configured (Linux standalone mode)")
endif()

# ==================================================================
# Shared library mode: build slic3r.dll/.so from engine source + libslic3r
#   Windows: cmake -DBUILD_SLIC3R_DLL=ON -DLIBSLIC3R_DIR=<path>
#   Linux:   cmake -DBUILD_SLIC3R_DLL=ON -DORCASLICER_BUILD_DIR=<path> -DORCASLICER_SOURCE_DIR=<path>
# On Linux this also builds the consumer executable (orca-slice-engine from main.c).
# ==================================================================
if(BUILD_SLIC3R_DLL)

# ---- Windows DLL mode ----
if(WIN32)
    set(LIBSLIC3R_DIR "" CACHE PATH "OrcaSlicer source directory")
    if(NOT LIBSLIC3R_DIR)
        message(FATAL_ERROR "LIBSLIC3R_DIR required. Pass -DLIBSLIC3R_DIR=<path>")
    endif()
    set(ORCASLICER_BUILD_DIR "${LIBSLIC3R_DIR}/build" CACHE PATH "OrcaSlicer CMake build directory (e.g. build or build_engine)")

    set(LIBSLIC3R_INCLUDE_DIRS
        ${LIBSLIC3R_DIR}/src
        ${LIBSLIC3R_DIR}/src/libslic3r
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include/boost-1_84
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include/occt
    )
    set(OCCT_LIB_DIR ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/lib/occt)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(LIBSLIC3R_LIBRARY ${ORCASLICER_BUILD_DIR}/src/libslic3r/Debug/libslic3r.lib)
    else()
        set(LIBSLIC3R_LIBRARY ${ORCASLICER_BUILD_DIR}/src/libslic3r/Release/libslic3r.lib)
    endif()
    if(NOT EXISTS "${LIBSLIC3R_LIBRARY}")
        message(FATAL_ERROR "libslic3r.lib not found at ${LIBSLIC3R_LIBRARY}. Build OrcaSlicer first.")
    endif()
    message(STATUS "Using libslic3r: ${LIBSLIC3R_LIBRARY}")

    add_library(slic3r SHARED
        src/slic3r_c_api.h src/slic3r_c_api.cpp
        src/SliceEngine.cpp src/JsonReport.cpp src/GeometryCheck.cpp
        src/Utils.cpp src/nanosvg.cpp
    )

    target_include_directories(slic3r PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIBSLIC3R_INCLUDE_DIRS})
    target_include_directories(slic3r SYSTEM PRIVATE
        ${LIBSLIC3R_DIR}/deps_src ${LIBSLIC3R_DIR}/deps_src/eigen
        ${LIBSLIC3R_DIR}/deps_src/nanosvg ${LIBSLIC3R_DIR}/deps_src/libigl
        ${LIBSLIC3R_DIR}/deps_src/ankerl ${LIBSLIC3R_DIR}/deps_src/fast_float
        ${LIBSLIC3R_DIR}/deps_src/nlohmann ${LIBSLIC3R_DIR}/deps_src/miniz
        ${LIBSLIC3R_DIR}/deps_src/qhull ${LIBSLIC3R_DIR}/deps_src/admesh
        ${LIBSLIC3R_DIR}/deps_src/clipper ${LIBSLIC3R_DIR}/deps_src/spline
    )

    target_compile_definitions(slic3r PRIVATE
        SLIC3R_DLL_EXPORTS USE_TBB TBB_USE_CAPTURED_EXCEPTION=0 _USE_MATH_DEFINES
    )

    if(MSVC)
        target_compile_options(slic3r PRIVATE "/UBOOST_ALL_DYN_LINK")
        target_compile_definitions(slic3r PRIVATE BOOST_ALL_STATIC_LINK)
        target_link_options(slic3r PRIVATE "/LIBPATH:${CMAKE_PREFIX_PATH}/lib")
        target_link_options(slic3r PRIVATE "/LIBPATH:${OCCT_LIB_DIR}")
    endif()

    target_link_libraries(slic3r PRIVATE
        ${LIBSLIC3R_LIBRARY}
        ${ORCASLICER_BUILD_DIR}/src/libslic3r/Release/libslic3r_cgal.lib
    )

    # deps_src static libs (transitive deps of libslic3r)
    set(DSB ${ORCASLICER_BUILD_DIR}/deps_src)
    set(DLB ${ORCASLICER_BUILD_DIR}/lib/Release)
    target_link_libraries(slic3r PRIVATE
        ${DLB}/semver.lib
        ${DSB}/clipper/Release/clipper.lib
        ${DSB}/qhull/Release/qhullstatic.lib
        ${DSB}/miniz/Release/miniz_static.lib
        ${DSB}/admesh/Release/admesh.lib
        ${DSB}/mcut/Release/mcut.lib
        ${DSB}/glu-libtess/Release/glu-libtess.lib
        ${DSB}/qoi/Release/qoi.lib
        ${DSB}/libnest2d/Release/libnest2d.lib
    )

    set(DEP_LIB ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/lib)
    target_link_libraries(slic3r PRIVATE
        ${DEP_LIB}/libnoise_static.lib ${DEP_LIB}/jpeg-static.lib
        ${DEP_LIB}/libpng16_static.lib ${DEP_LIB}/zlib.lib ${DEP_LIB}/freetype.lib
        ${DEP_LIB}/expat.lib
    )

    # CGAL (header-only — add include dirs directly to avoid find_package pulling vc143 Boost)
    target_include_directories(slic3r SYSTEM PRIVATE
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include
    )
    target_compile_definitions(slic3r PRIVATE BOOST_ALL_NO_LIB)

    # Boost (explicit paths — avoid auto-link vc143/vc144 mismatch)
    target_link_libraries(slic3r PRIVATE
        ${DEP_LIB}/libboost_filesystem-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_log-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_log_setup-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_nowide-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_locale-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_thread-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_chrono-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_atomic-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_container-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_context-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_coroutine-vc144-mt-x64-1_84.lib
        ${DEP_LIB}/libboost_date_time-vc144-mt-x64-1_84.lib
    )

    # TBB (explicit paths)
    target_link_libraries(slic3r PRIVATE
        ${DEP_LIB}/tbb12.lib
        ${DEP_LIB}/tbbmalloc.lib
    )

    # OpenSSL (explicit paths)
    target_link_libraries(slic3r PRIVATE
        ${DEP_LIB}/libssl.lib
        ${DEP_LIB}/libcrypto.lib
    )

    # System libs
    target_link_libraries(slic3r PRIVATE Psapi.lib bcrypt.lib)

    set(OCCT_LIBS TKG2d TKG3d TKMath TKernel TKGeomBase TKGeomAlgo TKBRep TKTopAlgo TKPrim TKHLR TKShHealing TKBO TKBool TKMesh TKXSBase TKSTEP TKSTEPAttr TKSTEPBase TKSTEP209 TKIGES TKSTL TKXml TKXmlL TKXmlXCAF TKLCAF TKCAF TKBinL TKXCAF TKBin TKBinXCAF TKVCAF TKCDF TKV3d TKService TKRWMesh TKVRML TKXDEIGES TKXDESTEP)
    foreach(lib ${OCCT_LIBS})
        target_link_libraries(slic3r PRIVATE ${OCCT_LIB_DIR}/${lib}.lib)
    endforeach()
    target_link_libraries(slic3r PRIVATE ${DEP_LIB}/libgmp-10.lib ${DEP_LIB}/libmpfr-4.lib)

    message(STATUS "slic3r.dll target enabled")
endif()

# ---- Linux SO mode: build libslic3r.so + consumer exe ----
if(UNIX AND NOT APPLE)
    set(ORCASLICER_BUILD_DIR "" CACHE PATH "OrcaSlicer build-engine directory")
    set(ORCASLICER_SOURCE_DIR "" CACHE PATH "OrcaSlicer source directory")

    if(NOT ORCASLICER_BUILD_DIR OR NOT ORCASLICER_SOURCE_DIR)
        message(FATAL_ERROR
            "Linux BUILD_SLIC3R_DLL requires:\n"
            "  -DORCASLICER_BUILD_DIR=<path/to/OrcaSlicer/build-engine>\n"
            "  -DORCASLICER_SOURCE_DIR=<path/to/OrcaSlicer>")
    endif()

    set(DEPS_DIR "${ORCASLICER_SOURCE_DIR}/deps/build/destdir/usr/local")
    set(BUILD_DEPS_SRC "${ORCASLICER_BUILD_DIR}/deps_src")
    set(BUILD_LIB "${ORCASLICER_BUILD_DIR}/lib")

    message(STATUS "OrcaSlicer build dir: ${ORCASLICER_BUILD_DIR}")
    message(STATUS "OrcaSlicer source dir: ${ORCASLICER_SOURCE_DIR}")

    # === Shared library collections (same as standalone block) ===
    set(ENGINE_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ORCASLICER_SOURCE_DIR}/src
        ${ORCASLICER_BUILD_DIR}/src
        ${ORCASLICER_SOURCE_DIR}/src/libslic3r
        ${ORCASLICER_BUILD_DIR}/src/libslic3r
    )

    set(DEPS_SRC_LIBS
        ${BUILD_DEPS_SRC}/libnest2d/liblibnest2d.a
        ${BUILD_DEPS_SRC}/miniz/libminiz_static.a
        ${BUILD_DEPS_SRC}/clipper/libclipper.a
        ${BUILD_DEPS_SRC}/glu-libtess/libglu-libtess.a
        ${BUILD_DEPS_SRC}/admesh/libadmesh.a
        ${BUILD_DEPS_SRC}/mcut/libmcut.a
        ${BUILD_DEPS_SRC}/qoi/libqoi.a
        ${BUILD_LIB}/libsemver.a
    )

    set(OCCT_LIBS
        ${DEPS_DIR}/lib/libTKXDESTEP.a
        ${DEPS_DIR}/lib/libTKSTEP.a
        ${DEPS_DIR}/lib/libTKSTEP209.a
        ${DEPS_DIR}/lib/libTKSTEPAttr.a
        ${DEPS_DIR}/lib/libTKSTEPBase.a
        ${DEPS_DIR}/lib/libTKXCAF.a
        ${DEPS_DIR}/lib/libTKXSBase.a
        ${DEPS_DIR}/lib/libTKVCAF.a
        ${DEPS_DIR}/lib/libTKCAF.a
        ${DEPS_DIR}/lib/libTKLCAF.a
        ${DEPS_DIR}/lib/libTKCDF.a
        ${DEPS_DIR}/lib/libTKV3d.a
        ${DEPS_DIR}/lib/libTKService.a
        ${DEPS_DIR}/lib/libTKMesh.a
        ${DEPS_DIR}/lib/libTKBO.a
        ${DEPS_DIR}/lib/libTKPrim.a
        ${DEPS_DIR}/lib/libTKHLR.a
        ${DEPS_DIR}/lib/libTKShHealing.a
        ${DEPS_DIR}/lib/libTKTopAlgo.a
        ${DEPS_DIR}/lib/libTKGeomAlgo.a
        ${DEPS_DIR}/lib/libTKBRep.a
        ${DEPS_DIR}/lib/libTKGeomBase.a
        ${DEPS_DIR}/lib/libTKG3d.a
        ${DEPS_DIR}/lib/libTKG2d.a
        ${DEPS_DIR}/lib/libTKMath.a
        ${DEPS_DIR}/lib/libTKernel.a
    )

    set(BOOST_LIBS
        ${DEPS_DIR}/lib/libboost_log_setup.a
        ${DEPS_DIR}/lib/libboost_log.a
        ${DEPS_DIR}/lib/libboost_filesystem.a
        ${DEPS_DIR}/lib/libboost_coroutine.a
        ${DEPS_DIR}/lib/libboost_context.a
        ${DEPS_DIR}/lib/libboost_serialization.a
        ${DEPS_DIR}/lib/libboost_locale.a
        ${DEPS_DIR}/lib/libboost_thread.a
        ${DEPS_DIR}/lib/libboost_chrono.a
        ${DEPS_DIR}/lib/libboost_atomic.a
        ${DEPS_DIR}/lib/libboost_date_time.a
        ${DEPS_DIR}/lib/libboost_exception.a
        ${DEPS_DIR}/lib/libboost_program_options.a
        ${DEPS_DIR}/lib/libboost_container.a
        ${DEPS_DIR}/lib/libboost_nowide.a
        ${DEPS_DIR}/lib/libboost_iostreams.a
        ${DEPS_DIR}/lib/libboost_random.a
    )

    set(MATH_LIBS
        ${DEPS_DIR}/lib/libgmpxx.a
        ${DEPS_DIR}/lib/libmpfr.a
        ${DEPS_DIR}/lib/libgmp.a
    )

    set(OTHER_DEPS_LIBS
        ${DEPS_DIR}/lib/liblibnoise_static.a
        ${DEPS_DIR}/lib/libpng.a
        ${DEPS_DIR}/lib/libqhullcpp.a
        ${DEPS_DIR}/lib/libqhullstatic_r.a
        ${DEPS_DIR}/lib/libjpeg.a
        ${DEPS_DIR}/lib/libfreetype.a
        ${DEPS_DIR}/lib/libopenvdb.a
        ${DEPS_DIR}/lib/libHalf-2_5.a
        ${DEPS_DIR}/lib/libblosc.a
        ${DEPS_DIR}/lib/libnlopt.a
    )

    set(ENGINE_SYSTEM_INCLUDE_DIRS
        ${DEPS_DIR}/include
        ${DEPS_DIR}/include/opencascade
        ${DEPS_DIR}/include/opencv4
        ${ORCASLICER_SOURCE_DIR}/deps_src/nlohmann
        ${ORCASLICER_SOURCE_DIR}/deps_src/nanosvg
        ${ORCASLICER_SOURCE_DIR}/deps_src/miniz
        ${ORCASLICER_SOURCE_DIR}/deps_src/eigen
        ${ORCASLICER_SOURCE_DIR}/deps_src/libigl
        ${ORCASLICER_SOURCE_DIR}/deps_src/admesh
        ${ORCASLICER_SOURCE_DIR}/deps_src/admesh/..
        ${ORCASLICER_SOURCE_DIR}/deps_src/libnest2d/include
    )

    # ---- libslic3r.so: shared library = C API wrapper + engine ----
    add_library(slic3r SHARED
        src/slic3r_c_api.h src/slic3r_c_api.cpp
        src/SliceEngine.cpp src/SliceEngine.hpp
        src/PresetManager.cpp src/PresetManager.hpp
        src/PresetRollback.cpp src/PresetRollback.h
        src/PlateProcessor.cpp src/PlateProcessor.hpp
        src/StatisticsBuilder.cpp src/StatisticsBuilder.hpp
        src/JsonReport.cpp src/JsonReport.hpp
        src/GeometryCheck.cpp src/GeometryCheck.hpp
        src/Utils.cpp src/Utils.hpp
        src/EngineContext.hpp src/EngineCLI.hpp src/EngineCLI.cpp
        src/ExitCodes.hpp src/Issue.hpp src/SliceStats.hpp
        src/TempFileGuard.hpp src/Types.hpp
        src/nanosvg.cpp
    )

    target_include_directories(slic3r PRIVATE ${ENGINE_INCLUDE_DIRS})
    target_include_directories(slic3r SYSTEM PRIVATE ${ENGINE_SYSTEM_INCLUDE_DIRS})

    target_compile_definitions(slic3r PRIVATE
        -DSLIC3R_DLL_EXPORTS
        -DUSE_TBB
        -DTBB_USE_CAPTURED_EXCEPTION=0
        -DBOOST_ALL_NO_LIB
    )

    target_link_libraries(slic3r PRIVATE
        ${ORCASLICER_BUILD_DIR}/src/libslic3r/liblibslic3r.a
        ${ORCASLICER_BUILD_DIR}/src/libslic3r/liblibslic3r_cgal.a
        ${DEPS_DIR}/lib/libtbb.a
        ${DEPS_DIR}/lib/libtbbmalloc.a
        ${DEPS_DIR}/lib/libssl.a
        ${DEPS_DIR}/lib/libcrypto.a
        ${CMAKE_DL_LIBS}
        ${DEPS_SRC_LIBS}
        ${DEPS_DIR}/lib/libopencv_world.a
        ${DEPS_DIR}/lib/opencv4/3rdparty/liblibjpeg-turbo.a
        ${DEPS_DIR}/lib/opencv4/3rdparty/liblibpng.a
        ${OCCT_LIBS}
        ${BOOST_LIBS}
        ${MATH_LIBS}
        ${OTHER_DEPS_LIBS}
        pthread rt m fontconfig expat z tiff lzma
    )

    set_target_properties(slic3r PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        VERSION 02.02.01
        SOVERSION 2
    )

    message(STATUS "libslic3r.so target enabled")

    # ---- Consumer executable: pure C main.c → links libslic3r.so ----
    add_executable(orca-slice-engine src/main.c)
    target_include_directories(orca-slice-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(orca-slice-engine PRIVATE slic3r)
    # Embed RPATH so the binary finds libslic3r.so in ../lib relative to itself
    set_target_properties(orca-slice-engine PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )

    message(STATUS "orca-slice-engine consumer target enabled")
endif()

endif()

# ==================================================================
# Consumer mode (default): pure C exe against prebuilt slic3r library.
# On Linux, point CMAKE_PREFIX_PATH to the directory containing
# libslic3r.so (e.g. the build-dll output or a prebuilt SDK).
# ==================================================================
if(NOT BUILD_SLIC3R_DLL AND NOT ORCA_STANDALONE)
    find_library(SLIC3R_LIBRARY slic3r
        HINTS "${CMAKE_PREFIX_PATH}/lib" "${CMAKE_PREFIX_PATH}/bin" "${CMAKE_PREFIX_PATH}"
        REQUIRED)
    message(STATUS "Found slic3r: ${SLIC3R_LIBRARY}")
    add_executable(orca-slice-engine src/main.c)
    target_include_directories(orca-slice-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(orca-slice-engine PRIVATE ${SLIC3R_LIBRARY})
    message(STATUS "orca-slice-engine configured (C API consumer)")
endif()
