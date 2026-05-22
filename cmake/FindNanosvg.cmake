# FindNanosvg.cmake
# nanosvg is a single-header library located in deps_src/nanosvg/nanosvg.h
# When SDK is bundled, the header is at <sdk>/include/nanosvg.h

find_path(NANOSVG_INCLUDE_DIR
    NAMES nanosvg.h
    HINTS ${CMAKE_PREFIX_PATH}/include
          ${LIBSLIC3R_SDK_ROOT}/include
    PATH_SUFFIXES nanosvg
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Nanosvg
    REQUIRED_VARS NANOSVG_INCLUDE_DIR
)

if(Nanosvg_FOUND AND NOT TARGET Nanosvg::nanosvg)
    add_library(Nanosvg::nanosvg INTERFACE IMPORTED)
    set_target_properties(Nanosvg::nanosvg PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NANOSVG_INCLUDE_DIR}"
    )
endif()
