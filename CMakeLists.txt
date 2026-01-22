cmake_minimum_required(VERSION 3.20)
project(Rigel VERSION 0.0.0 LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug or Release)" FORCE)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Rigel/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/Rigel/version.h
    @ONLY
)

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
find_package(imgui QUIET)

target_link_libraries(RigelLib
    PUBLIC
        spdlog::spdlog
        GLEW::GLEW
        glfw
        OpenGL::GL
        ryml::ryml
        stb::stb
        glm::glm
)

if (imgui_FOUND)
    target_link_libraries(RigelLib PUBLIC imgui::imgui)
    target_compile_definitions(RigelLib PRIVATE RIGEL_ENABLE_IMGUI IMGUI_IMPL_OPENGL_LOADER_GLEW)
endif()

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

option(RIGEL_BUILD_TESTS "Build Rigel tests" ON)
if (RIGEL_BUILD_TESTS)
    enable_testing()
    target_compile_definitions(RigelLib PRIVATE RIGEL_ENABLE_PROFILER)

    file(GLOB_RECURSE TEST_SOURCES
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )

    add_executable(Rigel_tests ${TEST_SOURCES})
    target_include_directories(Rigel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(Rigel_tests PRIVATE RigelLib)
    target_compile_definitions(Rigel_tests PRIVATE RIGEL_ENABLE_PROFILER)

    set(TEST_CASE_NAMES "")
    foreach(test_src ${TEST_SOURCES})
        file(READ "${test_src}" test_contents)
        string(REGEX MATCHALL "TEST_CASE\\(([A-Za-z0-9_]+)\\)" test_matches "${test_contents}")
        foreach(test_match ${test_matches})
            string(REGEX REPLACE "TEST_CASE\\(([A-Za-z0-9_]+)\\)" "\\1" test_name "${test_match}")
            list(APPEND TEST_CASE_NAMES "${test_name}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES TEST_CASE_NAMES)

    foreach(test_name ${TEST_CASE_NAMES})
        add_test(NAME Rigel_${test_name} COMMAND Rigel_tests --filter ${test_name})
    endforeach()
endif()
