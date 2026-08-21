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
# so the test target never hard-codes absolute paths. The project only supports
# the ORCA_STATIC (single static binary) mode, so the static boost archives are
# always used here.
target_include_directories(engine-tests PRIVATE ${ORCA_STATIC_INCLUDES})
target_link_libraries(engine-tests PRIVATE ${ORCA_STATIC_BOOST_LIBS})

target_sources(engine-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_link_libraries(engine-tests PRIVATE pthread)

add_test(NAME engine-tests COMMAND engine-tests)

# ============================================================================
# Integration test target — links the FULL libslic3r (unlike the lightweight
# engine-tests above). Exercises SliceEngine stages that need PresetBundle /
# real 3MF input / resources. Only built in ORCA_STATIC mode (needs libslic3r.a).
# Sources mirror the orca-slice-engine target (CMakeLists.txt:45-54) MINUS
# main.c / slic3r_c_api.cpp — the test drives SliceEngine directly in C++.
# ============================================================================
if(ORCA_STATIC)
    add_executable(engine-integration
        catch2/catch_amalgamated.cpp
        test_preset_substitution.cpp
        test_geometry_preprocess.cpp
        test_slice_e2e.cpp
        test_file_validation.cpp
        test_printer_reject.cpp
        test_filament_rollback.cpp
        test_layer_height.cpp
        test_path_conflict.cpp
        test_temp_mode.cpp
        test_boundary.cpp
        test_gcode_security.cpp
        test_filament_count.cpp
        test_unsupported.cpp
        test_accuracy.cpp
        ${CMAKE_SOURCE_DIR}/src/SliceEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/PresetRollback.cpp
        ${CMAKE_SOURCE_DIR}/src/JsonReport.cpp
        ${CMAKE_SOURCE_DIR}/src/GeometryCheck.cpp
        ${CMAKE_SOURCE_DIR}/src/Utils.cpp
        ${CMAKE_SOURCE_DIR}/src/nanosvg.cpp
    )

    # Reuse the exact include dirs / defines / flags / link list of the static
    # engine (see CMakeLists.txt:56-77). Drop non-existent include dirs.
    # Also add tests/catch2 (for <catch_amalgamated.hpp>) and tests/ itself.
    set(_int_inc_dirs
        "${CMAKE_CURRENT_SOURCE_DIR}/catch2"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src")
    foreach(_d IN LISTS ORCA_STATIC_INCLUDES)
        if(EXISTS "${_d}")
            list(APPEND _int_inc_dirs "${_d}")
        endif()
    endforeach()
    target_include_directories(engine-integration PRIVATE ${_int_inc_dirs})

    target_compile_definitions(engine-integration PRIVATE
        ${ORCA_STATIC_DEFINES}
        ENGINE_VERSION="02.01.05"
        ORCA_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/u1_preset_test.3mf"
        ORCA_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        ORCA_TEST_RESOURCES="${CMAKE_SOURCE_DIR}/package/resources/profiles"
    )

    target_compile_options(engine-integration PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fext-numeric-literals;-fsigned-char>)
    set_target_properties(engine-integration PROPERTIES
        CXX_STANDARD 17 CXX_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON)

    target_link_libraries(engine-integration PRIVATE ${ORCA_STATIC_LIBS})

    add_test(NAME engine-integration COMMAND engine-integration)

    add_custom_target(run-tests
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -R engine --output-on-failure
        DEPENDS engine-tests engine-integration
        COMMENT "Running engine-tests + engine-integration via CTest"
    )
endif()
