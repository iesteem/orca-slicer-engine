cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# === Find precompiled libslic3r SDK ===
# SDK path passed via -DCMAKE_PREFIX_PATH=/path/to/libslic3r-sdk
find_package(libslic3r REQUIRED)

# === Engine directly depends on CGAL ===
# GeometryCheck.cpp calls MeshBoolean::cgal::does_self_intersect()
# Must explicitly find CGAL; libslic3r's find_dependency(CGAL) runs first
# but we declare the direct dependency here for clarity
find_package(CGAL REQUIRED)

# Boost — engine uses BOOST_LOG_TRIVIAL, filesystem
find_package(Boost REQUIRED COMPONENTS filesystem log)

# nanosvg — single-header SVG parser for thumbnails
find_package(Nanosvg REQUIRED)

# === Sources ===
add_executable(orca-slice-engine
    src/main.cpp
    src/SliceEngine.cpp
    src/JsonReport.cpp
    src/GeometryCheck.cpp
    src/Utils.cpp
    src/nanosvg.cpp
)

# === Include directories ===
target_include_directories(orca-slice-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBSLIC3R_INCLUDE_DIRS}
    ${CGAL_INCLUDE_DIRS}
    ${NANOSVG_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
)

# === Compile definitions ===
target_compile_definitions(orca-slice-engine PRIVATE
    -DUSE_TBB
    -DTBB_USE_CAPTURED_EXCEPTION=0
    -DBOOST_ALL_NO_LIB
)

# === Link libraries ===
target_link_libraries(orca-slice-engine PRIVATE
    libslic3r::libslic3r
    CGAL::CGAL
    Boost::filesystem
    Boost::log
    Nanosvg::nanosvg
    TBB::tbb
    TBB::tbbmalloc
    OpenSSL::SSL
    OpenSSL::Crypto
)

# === Platform-specific ===
if(WIN32)
    target_link_libraries(orca-slice-engine PRIVATE
        Psapi.lib
        bcrypt.lib
    )
elseif(APPLE)
    target_link_libraries(orca-slice-engine PRIVATE
        "-liconv"
        "-framework IOKit"
        "-framework CoreFoundation"
    )
else()
    target_link_libraries(orca-slice-engine PRIVATE
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
endif()

# === Install ===
install(TARGETS orca-slice-engine RUNTIME DESTINATION bin)

message(STATUS "orca-slice-engine ${PROJECT_VERSION} configured (standalone)")
message(STATUS "  libslic3r SDK: ${libslic3r_DIR}")
