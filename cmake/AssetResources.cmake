function(target_embed_resources TARGET_NAME RESOURCE_DIR)
    file(GLOB_RECURSE RESOURCES CONFIGURE_DEPENDS "${RESOURCE_DIR}/*")

    set(GENERATED_SOURCES "")
    set(REGISTRY_ENTRIES "")
    set(REGISTRY_KEYS "")

    foreach(FILE_PATH ${RESOURCES})
        # Get the relative path (e.g., "subdir/image.png") for the lookup key
        file(RELATIVE_PATH REL_PATH "${RESOURCE_DIR}" "${FILE_PATH}")
        
        # Create a C-safe variable name
        string(REGEX REPLACE "[^a-zA-Z0-9]" "_" SAFE_NAME "${REL_PATH}")
        set(SYM_START "_binary_${SAFE_NAME}_start")
        set(SYM_END   "_binary_${SAFE_NAME}_end")

        # 3. Generate Assembly File
        set(ASM_FILE "${CMAKE_CURRENT_BINARY_DIR}/embedded/${SAFE_NAME}.S")
        file(WRITE "${ASM_FILE}" "
.section .rodata
.global ${SYM_START}
.global ${SYM_END}
.align 16
${SYM_START}:
    .incbin \"${FILE_PATH}\"
${SYM_END}:
        ")
        # Rebuild this ASM file if the source asset changes.
        set_source_files_properties(${ASM_FILE} PROPERTIES OBJECT_DEPENDS "${FILE_PATH}")
        list(APPEND GENERATED_SOURCES "${ASM_FILE}")

        # Add entry to the C++ map generator
        # We store: { "filename", { start_pointer, end_pointer } }
        string(APPEND REGISTRY_ENTRIES "    { \"${REL_PATH}\", { ${SYM_START}, ${SYM_END} } },\n")
        string(APPEND REGISTRY_KEYS "    \"${REL_PATH}\",\n")
        
        # We need to declare the externs in the header so the map can see them
        string(APPEND EXTERN_DECLS "extern const char ${SYM_START}[];\nextern const char ${SYM_END}[];\n")
    endforeach()

    # Generate the Header File (ResourceRegistry.h)
    set(HEADER_FILE "${CMAKE_CURRENT_BINARY_DIR}/include/ResourceRegistry.h")
    file(WRITE "${HEADER_FILE}" "
#pragma once
#include <string>
#include <unordered_map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

// Forward declarations of the ASM symbols
extern \"C\" {
${EXTERN_DECLS}
}

class ResourceRegistry {
public:
    static std::span<const char> Get(const std::string& path) {
        static const std::unordered_map<std::string, std::pair<const char*, const char*>> registry = {
${REGISTRY_ENTRIES}
        };

        auto it = registry.find(path);
        if (it == registry.end()) {
            throw std::runtime_error(\"Resource not found: \" + path);
        }
        
        // Return a span (view) of the memory
        return std::span<const char>(it->second.first, it->second.second - it->second.first);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> paths = {
${REGISTRY_KEYS}
        };
        return paths;
    }
};
    ")

    # Create an object library for these resources to avoid huge archive link lines
    add_library(${TARGET_NAME}_resources OBJECT ${GENERATED_SOURCES})

    # Allow the main target to find the generated header
    target_include_directories(${TARGET_NAME}_resources INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include")

    # Link the resource objects into the main target
    target_sources(${TARGET_NAME} PRIVATE $<TARGET_OBJECTS:${TARGET_NAME}_resources>)

endfunction()
