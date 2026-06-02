cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 1.0.0 LANGUAGES C CXX)

option(BUILD_SLIC3R_DLL "Build slic3r.dll from engine source (requires libslic3r SDK)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================================================================
# DLL mode: build slic3r.dll from engine source + libslic3r
#   cmake -S . -B build_dll -DBUILD_SLIC3R_DLL=ON
#     -DLIBSLIC3R_DIR=<OrcaSlicer source>
#     -DCMAKE_PREFIX_PATH=<OrcaSlicer deps install>
# ==================================================================
if(BUILD_SLIC3R_DLL)

    set(LIBSLIC3R_DIR "" CACHE PATH "OrcaSlicer source directory")
    if(NOT LIBSLIC3R_DIR)
        message(FATAL_ERROR "LIBSLIC3R_DIR required. Pass -DLIBSLIC3R_DIR=<path>")
    endif()

    set(LIBSLIC3R_INCLUDE_DIRS
        ${LIBSLIC3R_DIR}/src
        ${LIBSLIC3R_DIR}/src/libslic3r
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include/boost-1_84
        ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/include/occt
    )
    set(OCCT_LIB_DIR ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/lib/occt)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(LIBSLIC3R_LIBRARY ${LIBSLIC3R_DIR}/build/src/libslic3r/Debug/libslic3r.lib)
    else()
        set(LIBSLIC3R_LIBRARY ${LIBSLIC3R_DIR}/build/src/libslic3r/Release/libslic3r.lib)
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
        ${LIBSLIC3R_DIR}/build/src/libslic3r/Release/libslic3r_cgal.lib
    )

    # deps_src static libs (transitive deps of libslic3r)
    set(DSB ${LIBSLIC3R_DIR}/build/deps_src)
    set(DLB ${LIBSLIC3R_DIR}/build/lib/Release)
    target_link_libraries(slic3r PRIVATE
        ${DLB}/semver.lib
        ${DSB}/clipper/Release/clipper.lib
        ${DSB}/qhull/Release/qhullstatic.lib
        ${DSB}/miniz/Release/miniz_static.lib
        ${DSB}/admesh/Release/admesh.lib
        ${DSB}/mcut/Release/mcut.lib
        ${DSB}/glu-libtess/Release/glu-libtess.lib
        ${DSB}/qoi/Release/qoi.lib
        ${DSB}/expat/Release/expat.lib
        ${DSB}/libnest2d/Release/libnest2d.lib
        ${DSB}/minilzo/Release/minilzo_static.lib
    )

    set(DEP_LIB ${LIBSLIC3R_DIR}/deps/build/OrcaSlicer_dep/usr/local/lib)
    target_link_libraries(slic3r PRIVATE
        ${DEP_LIB}/libnoise_static.lib ${DEP_LIB}/jpeg-static.lib
        ${DEP_LIB}/libpng16_static.lib ${DEP_LIB}/zlib.lib ${DEP_LIB}/freetype.lib
    )

    set(Boost_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    find_package(CGAL REQUIRED)
    target_link_libraries(slic3r PRIVATE CGAL::CGAL)
    find_package(Boost REQUIRED COMPONENTS filesystem log log_setup nowide locale)
    target_link_libraries(slic3r PRIVATE Boost::filesystem Boost::log Boost::log_setup Boost::nowide Boost::locale)
    find_package(TBB REQUIRED)
    target_link_libraries(slic3r PRIVATE TBB::tbb TBB::tbbmalloc)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(slic3r PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    # OCCT
    set(OCCT_LIBS TKG2d TKG3d TKMath TKernel TKGeomBase TKGeomAlgo TKBRep TKTopAlgo TKPrim TKHLR TKShHealing TKBO TKBool TKMesh TKXSBase TKSTEP TKSTEPAttr TKSTEPBase TKSTEP209 TKIGES TKSTL TKXml TKXmlL TKXmlXCAF TKLCAF TKCAF TKBinL TKXCAF TKBin TKBinXCAF TKVCAF TKCDF TKV3d TKService TKRWMesh TKVRML TKXDEIGES TKXDESTEP)
    foreach(lib ${OCCT_LIBS})
        target_link_libraries(slic3r PRIVATE ${OCCT_LIB_DIR}/${lib}.lib)
    endforeach()
    target_link_libraries(slic3r PRIVATE ${CMAKE_PREFIX_PATH}/lib/libgmp-10.lib ${CMAKE_PREFIX_PATH}/lib/libmpfr-4.lib)
    if(WIN32)
        target_link_libraries(slic3r PRIVATE Psapi.lib bcrypt.lib)
    endif()

    message(STATUS "slic3r.dll target enabled")
endif()

# ==================================================================
# Consumer mode (default): pure C exe against prebuilt slic3r.dll
# ==================================================================
if(NOT BUILD_SLIC3R_DLL)
    find_library(SLIC3R_LIBRARY slic3r
        HINTS "${CMAKE_PREFIX_PATH}/lib" "${CMAKE_PREFIX_PATH}/bin" "${CMAKE_PREFIX_PATH}"
        REQUIRED)
    message(STATUS "Found slic3r: ${SLIC3R_LIBRARY}")
    add_executable(orca-slice-engine src/main.c)
    target_include_directories(orca-slice-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(orca-slice-engine PRIVATE ${SLIC3R_LIBRARY})
    message(STATUS "orca-slice-engine configured (C API consumer)")
endif()
