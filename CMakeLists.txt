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
find_package(imgui QUIET)

set(RIGEL_HAS_IMGUI OFF)
if (imgui_FOUND AND TARGET imgui::imgui)
    set(_imgui_include_candidates "")
    if (DEFINED imgui_INCLUDE_DIRS)
        list(APPEND _imgui_include_candidates ${imgui_INCLUDE_DIRS})
    endif()
    if (DEFINED imgui_INCLUDE_DIR)
        list(APPEND _imgui_include_candidates ${imgui_INCLUDE_DIR})
    endif()
    if (NOT _imgui_include_candidates)
        get_target_property(IMGUI_INCLUDE_DIRS imgui::imgui INTERFACE_INCLUDE_DIRECTORIES)
        if (IMGUI_INCLUDE_DIRS)
            list(APPEND _imgui_include_candidates ${IMGUI_INCLUDE_DIRS})
        endif()
    endif()
    foreach(dir IN LISTS _imgui_include_candidates)
        string(REGEX REPLACE "\\$<[^:>]+:([^>]+)>" "\\1" dir "${dir}")
        if (EXISTS "${dir}/imgui.h")
            set(RIGEL_HAS_IMGUI ON)
            break()
        endif()
    endforeach()
endif()

set(RIGEL_IMGUI_BINDINGS_DIR "")
if (RIGEL_HAS_IMGUI)
    set(_imgui_pkg_folder "")
    if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND DEFINED imgui_PACKAGE_FOLDER_DEBUG)
        set(_imgui_pkg_folder "${imgui_PACKAGE_FOLDER_DEBUG}")
    elseif (CMAKE_BUILD_TYPE STREQUAL "Release" AND DEFINED imgui_PACKAGE_FOLDER_RELEASE)
        set(_imgui_pkg_folder "${imgui_PACKAGE_FOLDER_RELEASE}")
    elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" AND DEFINED imgui_PACKAGE_FOLDER_RELWITHDEBINFO)
        set(_imgui_pkg_folder "${imgui_PACKAGE_FOLDER_RELWITHDEBINFO}")
    elseif (CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND DEFINED imgui_PACKAGE_FOLDER_MINSIZEREL)
        set(_imgui_pkg_folder "${imgui_PACKAGE_FOLDER_MINSIZEREL}")
    endif()

    if (_imgui_pkg_folder AND EXISTS "${_imgui_pkg_folder}/res/bindings/imgui_impl_glfw.h")
        set(RIGEL_IMGUI_BINDINGS_DIR "${_imgui_pkg_folder}/res/bindings")
    endif()

    if (NOT RIGEL_IMGUI_BINDINGS_DIR)
        set(RIGEL_HAS_IMGUI OFF)
    endif()
endif()

set(RIGEL_OPTIONAL_COMPONENTS_LIST "")
if (RIGEL_HAS_IMGUI)
    list(APPEND RIGEL_OPTIONAL_COMPONENTS_LIST "imgui")
endif()
if (RIGEL_OPTIONAL_COMPONENTS_LIST)
    list(JOIN RIGEL_OPTIONAL_COMPONENTS_LIST ", " RIGEL_OPTIONAL_COMPONENTS)
else()
    set(RIGEL_OPTIONAL_COMPONENTS "none")
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
)

if (RIGEL_HAS_IMGUI)
    target_link_libraries(RigelLib PUBLIC imgui::imgui)
    target_include_directories(RigelLib PUBLIC ${RIGEL_IMGUI_BINDINGS_DIR})
    target_compile_definitions(RigelLib PRIVATE RIGEL_ENABLE_IMGUI IMGUI_IMPL_OPENGL_LOADER_CUSTOM)
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
