# ============================================================================
# Unit tests for the cloud slicing engine
# ============================================================================
#
# Usage:
#   1. Download Catch2 single-header:
#        wget https://github.com/catchorg/Catch2/releases/download/v3.5.2/catch_amalgamated.hpp
#      Place it alongside this file or in a location added to the include path.
#
#   2. Add to the parent CMakeLists.txt (not done automatically):
#        add_subdirectory(tests)
#
#   3. Build and run:
#        cmake --build <build-dir> --target engine-tests
#        ctest --test-dir <build-dir> -R engine-tests
#
# NOTE: The main CMakeLists.txt must be configured with ORCASLICER_BUILD_DIR
#       and ORCASLICER_SOURCE_DIR for the tests to link against libslic3r.a
#       and its dependencies.  The ENGINE_INCLUDE_DIRS variable must also be
#       propagated (currently it is scoped to the parent directory).
# ============================================================================

enable_testing()

add_executable(engine-tests
    test_main.cpp
    test_utils.cpp
    test_types.cpp
)

target_include_directories(engine-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# These tests only exercise utility functions and inline factory helpers.
# They do NOT depend on libslic3r or any heavy dependencies.
# Linking against Utils.cpp is needed for format_time_hhmmss() and
# generate_output_path(), both of which live in Utils.cpp.
target_sources(engine-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_link_libraries(engine-tests PRIVATE
    boost_filesystem
    boost_log
    boost_log_setup
    boost_date_time
    pthread
)

add_test(NAME engine-tests COMMAND engine-tests)
