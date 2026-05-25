cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find slic3r shared library
# CMAKE_PREFIX_PATH must point to the directory containing slic3r.dll / slic3r.lib
find_library(SLIC3R_LIBRARY slic3r
    HINTS "${CMAKE_PREFIX_PATH}/lib"
          "${CMAKE_PREFIX_PATH}/bin"
          "${CMAKE_PREFIX_PATH}"
    REQUIRED
)
message(STATUS "Found slic3r: ${SLIC3R_LIBRARY}")

find_path(SLIC3R_INCLUDE_DIR slic3r_c_api.h
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_PREFIX_PATH}/include/libslic3r"
          "${CMAKE_PREFIX_PATH}/include"
          "${CMAKE_PREFIX_PATH}/../../../src/libslic3r"
    REQUIRED
)
message(STATUS "Found slic3r_c_api.h: ${SLIC3R_INCLUDE_DIR}")

# Engine — pure C, only main.c
add_executable(orca-slice-engine src/main.c)

target_include_directories(orca-slice-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SLIC3R_INCLUDE_DIR}
)

target_link_libraries(orca-slice-engine PRIVATE ${SLIC3R_LIBRARY})

# Copy DLL alongside executable for convenient runtime
if(WIN32)
    add_custom_command(TARGET orca-slice-engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SLIC3R_LIBRARY}/../slic3r.dll"
            "$<TARGET_FILE_DIR:orca-slice-engine>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SLIC3R_LIBRARY}"
            "$<TARGET_FILE_DIR:orca-slice-engine>"
    )
endif()

message(STATUS "orca-slice-engine configured (C API consumer)")
