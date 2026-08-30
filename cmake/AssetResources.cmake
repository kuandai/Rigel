function(rigel_snapshot_generated_resources OUTPUT_VARIABLE ROOT OUTPUT_DIRECTORY
        PYTHON_EXECUTABLE IMPORTER_SCRIPT EXPECTED_JAR_SHA256)
    execute_process(
        COMMAND "${PYTHON_EXECUTABLE}" "${IMPORTER_SCRIPT}"
            --root "${ROOT}" snapshot --output "${OUTPUT_DIRECTORY}"
            --jar-sha256 "${EXPECTED_JAR_SHA256}"
        RESULT_VARIABLE SNAPSHOT_RESULT
        OUTPUT_VARIABLE SNAPSHOT_PATH
        ERROR_VARIABLE SNAPSHOT_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT SNAPSHOT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Generated asset snapshot failed:\n${SNAPSHOT_PATH}${SNAPSHOT_ERROR}")
    endif()
    if(NOT IS_DIRECTORY "${SNAPSHOT_PATH}")
        message(FATAL_ERROR
            "Generated asset snapshot did not produce a resource root: ${SNAPSHOT_PATH}")
    endif()
    set(${OUTPUT_VARIABLE} "${SNAPSHOT_PATH}" PARENT_SCOPE)
endfunction()

function(rigel_synchronize_generated_resources OUTPUT_VARIABLE ROOT OUTPUT_DIRECTORY
        PYTHON_EXECUTABLE IMPORTER_SCRIPT SOURCE_JAR)
    execute_process(
        COMMAND "${PYTHON_EXECUTABLE}" "${IMPORTER_SCRIPT}"
            --root "${ROOT}" sync --jar "${SOURCE_JAR}"
        RESULT_VARIABLE SYNC_RESULT
        OUTPUT_VARIABLE SYNC_OUTPUT
        ERROR_VARIABLE SYNC_ERROR)
    if(NOT SYNC_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Cosmic Reach asset synchronization failed.\n"
            "${SYNC_OUTPUT}${SYNC_ERROR}")
    endif()
    string(REGEX MATCH "JAR SHA-256: ([0-9a-f]+)" JAR_DIGEST_LINE
        "${SYNC_OUTPUT}")
    set(EXPECTED_JAR_SHA256 "${CMAKE_MATCH_1}")
    string(LENGTH "${EXPECTED_JAR_SHA256}" JAR_DIGEST_LENGTH)
    if(NOT JAR_DIGEST_LENGTH EQUAL 64)
        message(FATAL_ERROR
            "Asset synchronization did not report a valid source JAR digest.\n"
            "${SYNC_OUTPUT}${SYNC_ERROR}")
    endif()
    rigel_snapshot_generated_resources(
        SNAPSHOT_ROOT
        "${ROOT}"
        "${OUTPUT_DIRECTORY}"
        "${PYTHON_EXECUTABLE}"
        "${IMPORTER_SCRIPT}"
        "${EXPECTED_JAR_SHA256}")
    set(${OUTPUT_VARIABLE} "${SNAPSHOT_ROOT}" PARENT_SCOPE)
endfunction()

function(target_embed_resources TARGET_NAME)
    set(RESOURCE_DIRS ${ARGN})
    if(NOT RESOURCE_DIRS)
        message(FATAL_ERROR "target_embed_resources requires at least one resource root")
    endif()

    set(REGISTRY_ENTRIES "")
    set(REGISTRY_KEYS "")
    set(EXTERN_DECLS "")
    set(ASSEMBLY_CONTENT ".section .rodata\n")
    set(RESOURCE_DEPENDENCIES "")
    set(LOGICAL_RESOURCE_PATHS "")

    foreach(RESOURCE_DIR IN LISTS RESOURCE_DIRS)
        if(NOT IS_DIRECTORY "${RESOURCE_DIR}")
            message(FATAL_ERROR "Resource root does not exist: ${RESOURCE_DIR}")
        endif()
        file(GLOB_RECURSE ROOT_RESOURCES CONFIGURE_DEPENDS "${RESOURCE_DIR}/*")
        list(SORT ROOT_RESOURCES)

        foreach(FILE_PATH IN LISTS ROOT_RESOURCES)
            if(IS_DIRECTORY "${FILE_PATH}")
                continue()
            endif()
            file(RELATIVE_PATH REL_PATH "${RESOURCE_DIR}" "${FILE_PATH}")
            list(FIND LOGICAL_RESOURCE_PATHS "${REL_PATH}" EXISTING_INDEX)
            if(NOT EXISTING_INDEX EQUAL -1)
                message(FATAL_ERROR
                    "Duplicate logical resource path '${REL_PATH}' appears in multiple roots")
            endif()
            list(APPEND LOGICAL_RESOURCE_PATHS "${REL_PATH}")
            list(APPEND RESOURCE_DEPENDENCIES "${FILE_PATH}")

            string(REGEX REPLACE "[^a-zA-Z0-9]" "_" SAFE_NAME "${REL_PATH}")
            string(SHA256 PATH_HASH "${REL_PATH}")
            string(SUBSTRING "${PATH_HASH}" 0 16 SHORT_HASH)
            set(SYMBOL_NAME "${SAFE_NAME}_${SHORT_HASH}")
            set(SYM_START "_binary_${SYMBOL_NAME}_start")
            set(SYM_END "_binary_${SYMBOL_NAME}_end")

            string(REPLACE "\\" "\\\\" INC_PATH "${FILE_PATH}")
            string(REPLACE "\"" "\\\"" INC_PATH "${INC_PATH}")
            string(APPEND ASSEMBLY_CONTENT
                ".global ${SYM_START}\n"
                ".global ${SYM_END}\n"
                ".align 16\n"
                "${SYM_START}:\n"
                "    .incbin \"${INC_PATH}\"\n"
                "${SYM_END}:\n")
            string(APPEND REGISTRY_ENTRIES
                "    { \"${REL_PATH}\", { ${SYM_START}, ${SYM_END} } },\n")
            string(APPEND REGISTRY_KEYS "    \"${REL_PATH}\",\n")
            string(APPEND EXTERN_DECLS
                "extern const char ${SYM_START}[];\n"
                "extern const char ${SYM_END}[];\n")
        endforeach()
    endforeach()

    set(EMBEDDED_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded")
    file(MAKE_DIRECTORY "${EMBEDDED_DIR}")
    set(ASM_FILE "${EMBEDDED_DIR}/${TARGET_NAME}_resources.S")
    file(WRITE "${ASM_FILE}" "${ASSEMBLY_CONTENT}")
    set_source_files_properties("${ASM_FILE}" PROPERTIES
        OBJECT_DEPENDS "${RESOURCE_DEPENDENCIES}")

    set(HEADER_FILE "${CMAKE_CURRENT_BINARY_DIR}/include/ResourceRegistry.h")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
    file(WRITE "${HEADER_FILE}" "
#pragma once
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern \"C\" {
${EXTERN_DECLS}}

class ResourceRegistry {
public:
    static std::span<const char> Get(const std::string& path) {
        static const std::unordered_map<std::string, std::pair<const char*, const char*>> registry = {
${REGISTRY_ENTRIES}        };

        auto it = registry.find(path);
        if (it == registry.end()) {
            throw std::runtime_error(\"Resource not found: \" + path);
        }
        return std::span<const char>(it->second.first, it->second.second - it->second.first);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> paths = {
${REGISTRY_KEYS}        };
        return paths;
    }
};
")

    add_library(${TARGET_NAME}_resources OBJECT "${ASM_FILE}")
    target_include_directories(${TARGET_NAME}_resources
        INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
    target_include_directories(${TARGET_NAME}
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include")
    target_sources(${TARGET_NAME} PRIVATE $<TARGET_OBJECTS:${TARGET_NAME}_resources>)
endfunction()
