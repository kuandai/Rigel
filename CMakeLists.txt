cmake_minimum_required(VERSION 3.20)
project(Rigel VERSION 0.0.0 LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(RIGEL_BUILD_TESTS "Build Rigel tests" ON)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug or Release)" FORCE)
endif()

file(GLOB_RECURSE SOURCES
    CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
)

list(REMOVE_ITEM SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
)

# Asset preparation and embedding
include(cmake/AssetResources.cmake)

set(RIGEL_COSMIC_REACH_JAR "" CACHE FILEPATH
    "Absolute path to a developer-provided Cosmic Reach JAR")
set(RIGEL_RESOURCE_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(RIGEL_RESOLVED_COSMIC_REACH_JAR "")
set(RIGEL_COSMIC_REACH_JAR_SOURCE "")
set(RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR_SHA256 "")
set(RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR "")
set(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS OFF)

if(RIGEL_COSMIC_REACH_JAR)
    set(RIGEL_RESOLVED_COSMIC_REACH_JAR "${RIGEL_COSMIC_REACH_JAR}")
    set(RIGEL_COSMIC_REACH_JAR_SOURCE "RIGEL_COSMIC_REACH_JAR")
elseif(DEFINED ENV{RIGEL_COSMIC_REACH_JAR} AND
       NOT "$ENV{RIGEL_COSMIC_REACH_JAR}" STREQUAL "")
    set(RIGEL_RESOLVED_COSMIC_REACH_JAR "$ENV{RIGEL_COSMIC_REACH_JAR}")
    set(RIGEL_COSMIC_REACH_JAR_SOURCE "environment variable")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.rigel/source/Cosmic-Reach.jar")
    set(RIGEL_RESOLVED_COSMIC_REACH_JAR
        "${CMAKE_CURRENT_SOURCE_DIR}/.rigel/source/Cosmic-Reach.jar")
    set(RIGEL_COSMIC_REACH_JAR_SOURCE "staged file")
endif()

if(RIGEL_RESOLVED_COSMIC_REACH_JAR)
    if(NOT IS_ABSOLUTE "${RIGEL_RESOLVED_COSMIC_REACH_JAR}")
        message(FATAL_ERROR
            "Cosmic Reach JAR path must be absolute: "
            "${RIGEL_RESOLVED_COSMIC_REACH_JAR}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${RIGEL_RESOLVED_COSMIC_REACH_JAR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rigel_assets.py")
    rigel_synchronize_generated_resources(
        RIGEL_GENERATED_RESOURCE_ROOT
        RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR_SHA256
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated-resource-snapshots"
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rigel_assets.py"
        "${RIGEL_RESOLVED_COSMIC_REACH_JAR}"
        "${CMAKE_CURRENT_BINARY_DIR}/embedded/RigelLib_resources.S")
    if(RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR_SHA256 STREQUAL
       "58a2cc3b79b5413cfa0f2e4ae3b37f44ed7f11a5e828de57f9f3f71599ac570e")
        set(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS ON)
        if(RIGEL_BUILD_TESTS)
            rigel_snapshot_synchronized_source_jar(
                RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR
                "${RIGEL_RESOLVED_COSMIC_REACH_JAR}"
                "${RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR_SHA256}"
                "${CMAKE_CURRENT_BINARY_DIR}/synchronized-source-jars")
        endif()
    endif()
    message(STATUS
        "Cosmic Reach assets prepared from ${RIGEL_COSMIC_REACH_JAR_SOURCE}: "
        "${RIGEL_RESOLVED_COSMIC_REACH_JAR}")
    list(APPEND RIGEL_RESOURCE_ROOTS "${RIGEL_GENERATED_RESOURCE_ROOT}")
else()
    message(STATUS
        "Cosmic Reach runtime assets have not been prepared. Source-only builds "
        "and tests remain available. To prepare interactive runtime assets, run:\n"
        "  python3 scripts/rigel_assets.py stage /path/to/Cosmic-Reach.jar\n"
        "then reconfigure Rigel, or configure with:\n"
        "  -DRIGEL_COSMIC_REACH_JAR=/absolute/path/Cosmic-Reach.jar")
endif()

add_library(RigelLib ${SOURCES})

target_compile_definitions(RigelLib PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:RIGEL_ENABLE_PROFILER>
)

target_embed_resources(RigelLib ${RIGEL_RESOURCE_ROOTS})

if(RIGEL_GENERATED_RESOURCE_ROOT)
    rigel_retire_generated_resource_snapshots(
        "${CMAKE_CURRENT_BINARY_DIR}/generated-resource-snapshots"
        "${RIGEL_GENERATED_RESOURCE_ROOT}"
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rigel_assets.py")
endif()

target_include_directories(RigelLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

add_executable(${PROJECT_NAME} src/main.cpp)

# Conan 2 generates these configs in build folder
find_package(spdlog REQUIRED)
find_package(glew REQUIRED)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ryml REQUIRED)
find_package(stb REQUIRED)
find_package(glm REQUIRED)
find_package(imgui REQUIRED)

if (NOT RIGEL_IMGUI_BINDINGS_DIR)
    message(FATAL_ERROR "The dependency toolchain must provide RIGEL_IMGUI_BINDINGS_DIR")
endif()

set(RIGEL_GIT_HASH "")
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    execute_process(
        COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
        RESULT_VARIABLE RIGEL_GIT_RESULT
        OUTPUT_VARIABLE RIGEL_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (NOT RIGEL_GIT_RESULT EQUAL 0)
        set(RIGEL_GIT_HASH "")
    endif()
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Rigel/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/Rigel/version.h
    @ONLY
)

target_link_libraries(RigelLib
    PUBLIC
        spdlog::spdlog
        GLEW::GLEW
        glfw
        ryml::ryml
        glm::glm
    PRIVATE
        OpenGL::GL
        stb::stb
        imgui::imgui
)

target_include_directories(RigelLib PRIVATE "${RIGEL_IMGUI_BINDINGS_DIR}")
target_compile_definitions(RigelLib PRIVATE RIGEL_ENABLE_IMGUI IMGUI_IMPL_OPENGL_LOADER_CUSTOM)

if (CMAKE_DL_LIBS)
    target_link_libraries(RigelLib PRIVATE ${CMAKE_DL_LIBS})
endif()

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        RigelLib
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

option(RIGEL_BUILD_BENCHMARKS "Build Rigel performance benchmarks" OFF)
if (RIGEL_BUILD_BENCHMARKS)
    add_executable(Rigel_block_model_meshing_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/block_model_meshing_benchmark.cpp
    )
    target_compile_definitions(Rigel_block_model_meshing_benchmark PRIVATE
        RIGEL_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        RIGEL_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
        RIGEL_BENCHMARK_SYSTEM="${CMAKE_SYSTEM_NAME}"
        RIGEL_BENCHMARK_PROCESSOR="${CMAKE_SYSTEM_PROCESSOR}"
    )
    target_link_libraries(Rigel_block_model_meshing_benchmark PRIVATE RigelLib)

    add_executable(Rigel_near_camera_visibility_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/NearCameraVisibilityBenchmark.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/near_camera_visibility_benchmark.cpp
    )
    target_include_directories(Rigel_near_camera_visibility_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
    )
    target_compile_definitions(Rigel_near_camera_visibility_benchmark PRIVATE
        RIGEL_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
    target_link_libraries(Rigel_near_camera_visibility_benchmark PRIVATE RigelLib)

    add_executable(Rigel_streaming_assessment_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/streaming_assessment_benchmark.cpp
    )
    target_include_directories(Rigel_streaming_assessment_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_compile_definitions(Rigel_streaming_assessment_benchmark PRIVATE
        RIGEL_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
    target_link_libraries(Rigel_streaming_assessment_benchmark PRIVATE RigelLib)
endif()

get_target_property(RIGEL_LIBRARY_COMPILE_DEFINITIONS RigelLib COMPILE_DEFINITIONS)

if (RIGEL_BUILD_TESTS)
    enable_testing()

    if(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS AND UNIX AND NOT APPLE)
        find_package(OpenGL REQUIRED COMPONENTS EGL)
    endif()

    file(GLOB_RECURSE TEST_SOURCES
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )

    set(PROFILER_TEST_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/tests/profiler_test.cpp)
    set(BLOCK_ASSET_FAILURE_TEST_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_asset_failure_test.cpp
    )
    set(ASSET_MANIFEST_VALIDATION_TEST_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_manifest_validation_test.cpp
    )
    set(PUBLIC_HEADER_CONSUMER_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_header_consumer.cpp
    )
    set(GENERATED_ASSET_INTEGRATION_TEST_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_asset_integration_test.cpp
    )
    set(APPLICATION_LAUNCH_INTEGRATION_TEST_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/application_launch_integration_test.cpp
    )
    list(REMOVE_ITEM TEST_SOURCES
        ${PROFILER_TEST_SOURCE}
        ${BLOCK_ASSET_FAILURE_TEST_SOURCE}
        ${ASSET_MANIFEST_VALIDATION_TEST_SOURCE}
        ${PUBLIC_HEADER_CONSUMER_SOURCE}
        ${GENERATED_ASSET_INTEGRATION_TEST_SOURCE}
        ${APPLICATION_LAUNCH_INTEGRATION_TEST_SOURCE}
    )

    add_executable(Rigel_tests
        ${TEST_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/NearCameraVisibilityBenchmark.cpp
    )
    target_include_directories(Rigel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence
        ${CMAKE_CURRENT_SOURCE_DIR}/src/render
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(Rigel_tests PRIVATE RigelLib)
    if(TARGET OpenGL::EGL)
        target_link_libraries(Rigel_tests PRIVATE OpenGL::EGL)
        target_compile_definitions(Rigel_tests PRIVATE RIGEL_TEST_HAS_EGL)
    endif()
    target_compile_definitions(Rigel_tests PRIVATE
        $<$<CONFIG:Debug>:RIGEL_EXPECT_PROFILER_ENABLED>
        RIGEL_TEST_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
        RIGEL_TEST_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    )

    add_executable(Rigel_profiler_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFramework.cpp
        ${PROFILER_TEST_SOURCE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Profiler.cpp
    )
    target_include_directories(Rigel_profiler_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(Rigel_profiler_tests PRIVATE RIGEL_ENABLE_PROFILER)
    target_compile_definitions(Rigel_profiler_tests PRIVATE
        RIGEL_TEST_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(Rigel_block_asset_failure_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFramework.cpp
        ${BLOCK_ASSET_FAILURE_TEST_SOURCE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/BlockLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/BlockRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/TextureAtlas.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/WorldResources.cpp
    )
    target_include_directories(Rigel_block_asset_failure_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing_block_assets
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(Rigel_block_asset_failure_tests PRIVATE STB_IMAGE_IMPLEMENTATION)
    target_compile_definitions(Rigel_block_asset_failure_tests PRIVATE
        RIGEL_TEST_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(Rigel_block_asset_failure_tests PRIVATE
        spdlog::spdlog
        GLEW::GLEW
        OpenGL::GL
        ryml::ryml
        stb::stb
    )

    add_executable(Rigel_asset_manifest_validation_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFramework.cpp
        ${ASSET_MANIFEST_VALIDATION_TEST_SOURCE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/asset/AssetManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/asset/RawLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/asset/TextureLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/asset/ShaderLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/asset/ShaderCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/asset/ShaderAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/ChunkSerializer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/FormatRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/PersistenceService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/Storage.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/WorldSettings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/ZoneIdentifier.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence/backends/memory/MemoryFormat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/BlockRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/BlockGalleryChunkGenerator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/Chunk.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/ChunkManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/DensityFunction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/GeneratorDefinition.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/GeneratorDefinitionLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/Noise.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/WorldGenerator.cpp
    )
    target_include_directories(Rigel_asset_manifest_validation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/invalid_generator_manifests
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(Rigel_asset_manifest_validation_tests PRIVATE
        RIGEL_TEST_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(Rigel_asset_manifest_validation_tests PRIVATE
        spdlog::spdlog
        GLEW::GLEW
        OpenGL::GL
        ryml::ryml
        stb::stb
        glm::glm
    )

    add_executable(Rigel_public_header_consumer
        ${PUBLIC_HEADER_CONSUMER_SOURCE}
    )
    target_link_libraries(Rigel_public_header_consumer PRIVATE RigelLib)

    if(RIGEL_RESOLVED_COSMIC_REACH_JAR)
        add_executable(Rigel_generated_asset_integration_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFramework.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/GalleryCapturePublication.cpp
            ${GENERATED_ASSET_INTEGRATION_TEST_SOURCE}
        )
        target_include_directories(Rigel_generated_asset_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_definitions(Rigel_generated_asset_integration_tests PRIVATE
            RIGEL_TEST_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(Rigel_generated_asset_integration_tests PRIVATE RigelLib)
        if(TARGET OpenGL::EGL)
            target_link_libraries(
                Rigel_generated_asset_integration_tests PRIVATE OpenGL::EGL)
            target_compile_definitions(
                Rigel_generated_asset_integration_tests PRIVATE RIGEL_TEST_HAS_EGL)
        endif()
        if(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS)
            target_compile_definitions(
                Rigel_generated_asset_integration_tests PRIVATE
                RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS)
        endif()

        add_executable(Rigel_application_launch_integration_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFramework.cpp
            ${APPLICATION_LAUNCH_INTEGRATION_TEST_SOURCE}
        )
        target_include_directories(
            Rigel_application_launch_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_definitions(
            Rigel_application_launch_integration_tests PRIVATE
            RIGEL_TEST_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(
            Rigel_application_launch_integration_tests PRIVATE RigelLib)
        if(TARGET OpenGL::EGL)
            target_link_libraries(
                Rigel_application_launch_integration_tests PRIVATE OpenGL::EGL)
            target_compile_definitions(
                Rigel_application_launch_integration_tests PRIVATE
                RIGEL_TEST_HAS_EGL)
        endif()
        if(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS)
            target_compile_definitions(
                Rigel_application_launch_integration_tests PRIVATE
                RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS)
        endif()
    endif()

    add_dependencies(Rigel_tests
        Rigel_profiler_tests
        Rigel_block_asset_failure_tests
        Rigel_asset_manifest_validation_tests
        Rigel_public_header_consumer
    )
    if(TARGET Rigel_generated_asset_integration_tests)
        add_dependencies(Rigel_tests Rigel_generated_asset_integration_tests)
    endif()
    if(TARGET Rigel_application_launch_integration_tests)
        add_dependencies(
            Rigel_tests Rigel_application_launch_integration_tests)
    endif()

    add_test(NAME Rigel_tests COMMAND Rigel_tests)
    add_test(NAME Rigel_profiler_tests COMMAND Rigel_profiler_tests)
    add_test(NAME Rigel_block_asset_failure_tests COMMAND Rigel_block_asset_failure_tests)
    add_test(NAME Rigel_asset_manifest_validation_tests COMMAND Rigel_asset_manifest_validation_tests)
    add_test(NAME Rigel_public_header_consumer COMMAND Rigel_public_header_consumer)
    if(TARGET Rigel_generated_asset_integration_tests)
        add_test(
            NAME Rigel_generated_asset_integration_tests
            COMMAND Rigel_generated_asset_integration_tests)
        if(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS)
            set_tests_properties(
                Rigel_generated_asset_integration_tests PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "Ran [1-9][0-9]* tests, 0 failed, 0 skipped\\."
            )
        endif()
        if(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS AND UNIX AND NOT APPLE)
            set_tests_properties(
                Rigel_generated_asset_integration_tests PROPERTIES
                ENVIRONMENT
                    "DISPLAY=;WAYLAND_DISPLAY=;LIBGL_ALWAYS_SOFTWARE=1")
        elseif(UNIX AND NOT APPLE)
            set_tests_properties(
                Rigel_generated_asset_integration_tests PROPERTIES
                ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
        endif()
    endif()
    if(TARGET Rigel_application_launch_integration_tests)
        add_test(
            NAME Rigel_application_launch_integration_tests
            COMMAND Rigel_application_launch_integration_tests)
        if(TARGET OpenGL::EGL AND UNIX AND NOT APPLE)
            set_tests_properties(
                Rigel_application_launch_integration_tests PROPERTIES
                ENVIRONMENT
                    "DISPLAY=;WAYLAND_DISPLAY=;LIBGL_ALWAYS_SOFTWARE=1")
        endif()
    endif()
    add_test(
        NAME Rigel_resource_roots
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_RESOURCES_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssetResources.cmake"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/resource-roots-test"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_roots_test.cmake
    )
    add_test(
        NAME Rigel_asset_importer_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v
            tests/rigel_assets_test.py
    )
    set_tests_properties(Rigel_asset_importer_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "RIGEL_COSMIC_REACH_JAR=")
    if(RIGEL_EXPECT_COSMIC_REACH_0_6_1_ASSETS)
        add_test(
            NAME Rigel_real_asset_importer_tests
            COMMAND ${CMAKE_COMMAND} -E env
                "RIGEL_COSMIC_REACH_JAR=${RIGEL_SYNCHRONIZED_COSMIC_REACH_JAR}"
                "${Python3_EXECUTABLE}" -m unittest -v
                tests.rigel_assets_test.RealJarBlockModelClosureTest)
        set_tests_properties(Rigel_real_asset_importer_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_test(
        NAME Rigel_tests_filtered_count
        COMMAND Rigel_tests --filter AssetHandle_Basics
    )
    set_tests_properties(Rigel_tests_filtered_count PROPERTIES
        PASS_REGULAR_EXPRESSION "Ran 1 tests, 0 failed, 0 skipped\\."
    )
    if (RIGEL_BUILD_BENCHMARKS)
        add_test(
            NAME Rigel_block_model_meshing_benchmark_options
            COMMAND ${CMAKE_COMMAND}
                "-DBENCHMARK_EXECUTABLE=$<TARGET_FILE:Rigel_block_model_meshing_benchmark>"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_model_meshing_benchmark_options.cmake
        )
        add_test(
            NAME Rigel_near_camera_visibility_cadence_options
            COMMAND ${CMAKE_COMMAND}
                "-DBENCHMARK_EXECUTABLE=$<TARGET_FILE:Rigel_near_camera_visibility_benchmark>"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/near_camera_visibility_cadence_options.cmake
        )
        add_test(
            NAME Rigel_streaming_assessment_cpu_contract
            COMMAND Rigel_streaming_assessment_benchmark
                --overlay-cpu-only --frames 3
        )
        set_tests_properties(
            Rigel_streaming_assessment_cpu_contract
            PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "requested_pair_count=3 disabled_sample_count=3 enabled_sample_count=3 pair_cardinality_asserted=true.*view_radius=12.*draw_evidence_records=1.*presentation_maps_and_meshes=true.*gl_calls=none.*imgui_legend_rendering=false.*workload=startup_backlog.*startup_state=streaming startup_non_quiescent_classified_backlog=true startup_loader_missing_max_distance_squared=1 startup_loader_outside=queued_indefinitely startup_pending_callback_matches_loader=true startup_retained_load_owner=load_pending.*startup_waiting_for_data=[1-9][0-9]*.*startup_waiting_for_neighbors=[1-9][0-9]*.*startup_mesh_scheduler_wait=0 startup_mesh_work=0.*startup_terminal_failure=0.*startup_generation_started=[1-9][0-9]* startup_generation_completed=[1-9][0-9]* startup_generation_cancelled=0 startup_generation_failed=0 startup_generation_pending=0 startup_generation_in_flight=0 startup_generation_completion_pending=0 startup_generation_terminal_failures=0 startup_canonical_source_work=0 startup_canonical_generation_work=0 startup_canonical_retired_work=0.*startup_chunk_load_in_flight=0 startup_chunk_load_terminal_failures=0.*startup_mesh_in_flight=0 startup_mesh_completion_pending=0 startup_mesh_terminal_failures=0 startup_eviction_pending=0 startup_eviction_in_flight=0 startup_eviction_terminal_failures=0 startup_execution_settled=true startup_logical_backlog_classified=true"
                TIMEOUT 60
        )
        add_test(
            NAME Rigel_streaming_assessment_options
            COMMAND ${CMAKE_COMMAND}
                "-DBENCHMARK_EXECUTABLE=$<TARGET_FILE:Rigel_streaming_assessment_benchmark>"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/streaming_assessment_options.cmake
        )
    endif()

    get_target_property(RIGEL_LIBRARY_TEST_COMPILE_DEFINITIONS RigelLib COMPILE_DEFINITIONS)
    if (NOT RIGEL_LIBRARY_TEST_COMPILE_DEFINITIONS STREQUAL RIGEL_LIBRARY_COMPILE_DEFINITIONS)
        message(FATAL_ERROR "Test targets must not change RigelLib compile definitions")
    endif()
endif()
