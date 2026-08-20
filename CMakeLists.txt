cmake_minimum_required(VERSION 3.20)
project(Rigel VERSION 0.0.0 LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# Asset embed
include(cmake/AssetResources.cmake)

add_library(RigelLib ${SOURCES})

target_compile_definitions(RigelLib PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:RIGEL_ENABLE_PROFILER>
)

target_embed_resources(RigelLib "${CMAKE_CURRENT_SOURCE_DIR}/assets")

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
        OpenGL::GL
        ryml::ryml
        stb::stb
        glm::glm
        imgui::imgui
)

target_include_directories(RigelLib PRIVATE "${RIGEL_IMGUI_BINDINGS_DIR}")
target_compile_definitions(RigelLib PRIVATE RIGEL_ENABLE_IMGUI IMGUI_IMPL_OPENGL_LOADER_CUSTOM)

if (CMAKE_DL_LIBS)
    target_link_libraries(RigelLib PUBLIC ${CMAKE_DL_LIBS})
endif()

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        RigelLib
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

get_target_property(RIGEL_LIBRARY_COMPILE_DEFINITIONS RigelLib COMPILE_DEFINITIONS)

option(RIGEL_BUILD_TESTS "Build Rigel tests" ON)
if (RIGEL_BUILD_TESTS)
    enable_testing()

    file(GLOB_RECURSE TEST_SOURCES
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )

    set(PROFILER_TEST_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/tests/profiler_test.cpp)
    set(BLOCK_ASSET_FAILURE_TEST_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_asset_failure_test.cpp
    )
    list(REMOVE_ITEM TEST_SOURCES
        ${PROFILER_TEST_SOURCE}
        ${BLOCK_ASSET_FAILURE_TEST_SOURCE}
    )

    add_executable(Rigel_tests ${TEST_SOURCES})
    target_include_directories(Rigel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(Rigel_tests PRIVATE RigelLib)
    target_compile_definitions(Rigel_tests PRIVATE
        $<$<CONFIG:Debug>:RIGEL_EXPECT_PROFILER_ENABLED>
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
    target_link_libraries(Rigel_block_asset_failure_tests PRIVATE
        spdlog::spdlog
        GLEW::GLEW
        OpenGL::GL
        ryml::ryml
        stb::stb
    )

    add_dependencies(Rigel_tests
        Rigel_profiler_tests
        Rigel_block_asset_failure_tests
    )

    add_test(NAME Rigel_tests COMMAND Rigel_tests)
    add_test(NAME Rigel_profiler_tests COMMAND Rigel_profiler_tests)
    add_test(NAME Rigel_block_asset_failure_tests COMMAND Rigel_block_asset_failure_tests)
    add_test(
        NAME Rigel_tests_filtered_count
        COMMAND Rigel_tests --filter AssetHandle_Basics
    )
    set_tests_properties(Rigel_tests_filtered_count PROPERTIES
        PASS_REGULAR_EXPRESSION "Ran 1 tests, 0 failed, 0 skipped\\."
    )

    get_target_property(RIGEL_LIBRARY_TEST_COMPILE_DEFINITIONS RigelLib COMPILE_DEFINITIONS)
    if (NOT RIGEL_LIBRARY_TEST_COMPILE_DEFINITIONS STREQUAL RIGEL_LIBRARY_COMPILE_DEFINITIONS)
        message(FATAL_ERROR "Test targets must not change RigelLib compile definitions")
    endif()
endif()
