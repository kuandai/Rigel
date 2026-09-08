if(NOT DEFINED SEMANTIC_EXECUTABLE)
    message(FATAL_ERROR "SEMANTIC_EXECUTABLE is required")
endif()

if(APPLE)
    execute_process(
        COMMAND otool -L "${SEMANTIC_EXECUTABLE}"
        RESULT_VARIABLE INSPECT_RESULT
        OUTPUT_VARIABLE LINKAGE)
elseif(UNIX)
    execute_process(
        COMMAND ldd "${SEMANTIC_EXECUTABLE}"
        RESULT_VARIABLE INSPECT_RESULT
        OUTPUT_VARIABLE LINKAGE)
else()
    message(STATUS "Graphics linkage inspection is unavailable on this platform")
    return()
endif()

if(NOT INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to inspect semantic executable linkage")
endif()

string(TOLOWER "${LINKAGE}" LOWER_LINKAGE)
if(LOWER_LINKAGE MATCHES "lib(gl|egl|glfw|glew)[^a-z]")
    message(FATAL_ERROR "Semantic executable has graphics linkage:\n${LINKAGE}")
endif()

message(STATUS "Semantic executable has no GL, EGL, GLFW, or GLEW linkage")
