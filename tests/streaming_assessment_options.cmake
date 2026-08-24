if (NOT DEFINED BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "BENCHMARK_EXECUTABLE is required")
endif()

function(assert_rejected EXPECTED_ERROR)
    execute_process(
        COMMAND "${BENCHMARK_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE RESULT
        OUTPUT_VARIABLE OUTPUT
        ERROR_VARIABLE ERROR
    )
    if (RESULT EQUAL 0)
        message(FATAL_ERROR "Assessment accepted invalid arguments: ${ARGN}")
    endif()
    if (NOT ERROR MATCHES "${EXPECTED_ERROR}")
        message(FATAL_ERROR
            "Assessment misdiagnosed '${ARGN}': ${ERROR}")
    endif()
    if (OUTPUT MATCHES "benchmark name=")
        message(FATAL_ERROR
            "Rejected assessment arguments emitted evidence: ${ARGN}")
    endif()
endfunction()

assert_rejected("Invalid bounded frame count" --frames 3x)
assert_rejected("Invalid bounded frame count"
    --frames 18446744073709551616)
assert_rejected("Invalid bounded frame count" --frames 10001)
assert_rejected("Frame count may be specified only once"
    --frames 3 --frames 4)
assert_rejected("Only one assessment mode may be selected"
    --overlay-only --overlay-cpu-only)
assert_rejected("Frame count is unsupported for vertical-only mode"
    --vertical-only --frames 3)
