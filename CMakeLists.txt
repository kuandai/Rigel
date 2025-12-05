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

# Asset embed
include(cmake/AssetResources.cmake)

add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_definitions(Rigel PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
)

target_embed_resources(${PROJECT_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/assets")

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

# Conan 2 generates these configs in build folder
find_package(spdlog REQUIRED)
find_package(glew REQUIRED)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        spdlog::spdlog
        GLEW::GLEW
        glfw
        OpenGL::GL
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
