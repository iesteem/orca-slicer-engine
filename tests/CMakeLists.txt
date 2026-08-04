# ============================================================================
# Unit tests for the cloud slicing engine
# ============================================================================
#
# Catch2 v3.5.2 is vendored as the single-header amalgamation under
# tests/catch2/ (catch_amalgamated.hpp + catch_amalgamated.cpp). The .cpp
# translation unit unconditionally supplies `main()`, so no CATCH_CONFIG_MAIN
# define or separate test_main.cpp is needed — test files just include
# <catch_amalgamated.hpp>.
#
# Build and run:
#   cmake --build <build-dir> --target engine-tests
#   ctest --test-dir <build-dir> -R engine-tests --output-on-failure
#
# These tests only exercise pure helpers that do NOT depend on libslic3r:
#   - Utils.cpp:            format_time_hhmmss(), generate_output_path()
#   - Types.hpp:            Issue factory functions (make_error/warning/tip)
#   - PlateGrid.hpp:        compute_column_count(), plate_grid_cell(),
#                           compute_plate_origin() (extracted from SliceEngine)
#   - SlicingDeadline.hpp:  deadline_expired() (extracted from SliceEngine)
#   - BuildVolumeClassify.hpp: classify_build_volume_issues() (from SliceEngine)
#   - ValidateClassify.hpp: classify_validate_exception() (from SliceEngine)
# Utils.cpp is the only .cpp linked (for format_time_hhmmss/generate_output_path);
# the rest are header-only inline. gc-sections drops Utils.cpp's unused
# libslic3r references so the test binary stays free of libslic3r.
# ============================================================================

add_executable(engine-tests
    catch2/catch_amalgamated.cpp
    test_utils.cpp
    test_types.cpp
    test_plate_grid.cpp
    test_slicing_deadline.cpp
    test_build_volume_classify.cpp
    test_validate_classify.cpp
)

# Utils.cpp also contains resize_thumbnail()/format_exception_context() which
# reference libslic3r symbols (ThumbnailData, ModelObject). The tests only
# exercise the pure helpers (format_time_hhmmss, compute_column_count,
# generate_output_path), so drop unreferenced functions at link time via
# --gc-sections instead of dragging in all of libslic3r.
target_compile_options(engine-tests PRIVATE -ffunction-sections -fdata-sections)
target_link_options(engine-tests PRIVATE -Wl,--gc-sections)

target_include_directories(engine-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/catch2     # <catch_amalgamated.hpp>
    ${CMAKE_CURRENT_SOURCE_DIR}            # test_utils.hpp (if added later)
    ${CMAKE_SOURCE_DIR}/src                # Utils.hpp, Types.hpp
)

# Boost headers + archives (Utils.hpp includes <boost/filesystem.hpp>). On this
# host Boost is built privately under the OrcaSlicer deps tree; reuse the same
# headers and archives the static engine uses (captured in StaticConfig.cmake),
# so the test target never hard-codes absolute paths.
if(ORCA_STATIC)
    target_include_directories(engine-tests PRIVATE ${ORCA_STATIC_INCLUDES})
    target_link_libraries(engine-tests PRIVATE ${ORCA_STATIC_BOOST_LIBS})
else()
    find_package(Boost REQUIRED COMPONENTS filesystem log log_setup)
    target_link_libraries(engine-tests PRIVATE Boost::filesystem Boost::log Boost::log_setup)
endif()

target_sources(engine-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_link_libraries(engine-tests PRIVATE pthread)

add_test(NAME engine-tests COMMAND engine-tests)
