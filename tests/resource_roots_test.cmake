if(NOT DEFINED ASSET_RESOURCES_MODULE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ASSET_RESOURCES_MODULE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY
    "${TEST_ROOT}/success/root-a/alpha"
    "${TEST_ROOT}/success/root-b/beta")
file(WRITE "${TEST_ROOT}/success/root-a/alpha/one.txt" "one")
file(WRITE "${TEST_ROOT}/success/root-b/beta/two.txt" "two")
file(WRITE "${TEST_ROOT}/success/dummy.cpp" "int resource_root_dummy() { return 0; }\n")
file(WRITE "${TEST_ROOT}/success/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
project(ResourceRootSuccess LANGUAGES CXX ASM)
include(\"${ASSET_RESOURCES_MODULE}\")
add_library(Dummy STATIC dummy.cpp)
target_embed_resources(Dummy
    \"${TEST_ROOT}/success/root-a\"
    \"${TEST_ROOT}/success/root-b\")
")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${TEST_ROOT}/success"
        -B "${TEST_ROOT}/success-build"
    RESULT_VARIABLE SUCCESS_CONFIGURE_RESULT
    OUTPUT_VARIABLE SUCCESS_CONFIGURE_OUTPUT
    ERROR_VARIABLE SUCCESS_CONFIGURE_ERROR)
if(NOT SUCCESS_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Multi-root configure failed:\n${SUCCESS_CONFIGURE_OUTPUT}\n${SUCCESS_CONFIGURE_ERROR}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_ROOT}/success-build"
        --target Dummy_resources
    RESULT_VARIABLE SUCCESS_BUILD_RESULT
    OUTPUT_VARIABLE SUCCESS_BUILD_OUTPUT
    ERROR_VARIABLE SUCCESS_BUILD_ERROR)
if(NOT SUCCESS_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Multi-root resource build failed:\n${SUCCESS_BUILD_OUTPUT}\n${SUCCESS_BUILD_ERROR}")
endif()
file(READ "${TEST_ROOT}/success-build/include/ResourceRegistry.h" GENERATED_HEADER)
if(NOT GENERATED_HEADER MATCHES "alpha/one.txt" OR
   NOT GENERATED_HEADER MATCHES "beta/two.txt")
    message(FATAL_ERROR "Generated registry did not contain both logical roots")
endif()

file(MAKE_DIRECTORY
    "${TEST_ROOT}/collision/root-a/shared"
    "${TEST_ROOT}/collision/root-b/shared")
file(WRITE "${TEST_ROOT}/collision/root-a/shared/item.txt" "one")
file(WRITE "${TEST_ROOT}/collision/root-b/shared/item.txt" "two")
file(WRITE "${TEST_ROOT}/collision/dummy.cpp" "int collision_dummy() { return 0; }\n")
file(WRITE "${TEST_ROOT}/collision/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
project(ResourceRootCollision LANGUAGES CXX ASM)
include(\"${ASSET_RESOURCES_MODULE}\")
add_library(Dummy STATIC dummy.cpp)
target_embed_resources(Dummy
    \"${TEST_ROOT}/collision/root-a\"
    \"${TEST_ROOT}/collision/root-b\")
")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${TEST_ROOT}/collision"
        -B "${TEST_ROOT}/collision-build"
    RESULT_VARIABLE COLLISION_RESULT
    OUTPUT_VARIABLE COLLISION_OUTPUT
    ERROR_VARIABLE COLLISION_ERROR)
if(COLLISION_RESULT EQUAL 0)
    message(FATAL_ERROR "Duplicate logical resource paths were accepted")
endif()
set(COLLISION_DIAGNOSTIC "${COLLISION_OUTPUT}\n${COLLISION_ERROR}")
if(NOT COLLISION_DIAGNOSTIC MATCHES "Duplicate logical resource path 'shared/item.txt'")
    message(FATAL_ERROR "Collision failure was not actionable:\n${COLLISION_DIAGNOSTIC}")
endif()
