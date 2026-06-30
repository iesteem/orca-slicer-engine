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

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Target 1: slic3r.dll (opt-in, needs OrcaSlicer SDK) ===
if(BUILD_SLIC3R_DLL)
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

# === Target 2: orca-slice-engine.exe (consumer, always built) ===

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

message(STATUS "orca-slice-engine configured (C API consumer)")
