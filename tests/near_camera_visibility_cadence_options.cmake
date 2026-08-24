foreach(INVALID_INTERVAL IN ITEMS 0.0000001 1e300)
    execute_process(
        COMMAND "${BENCHMARK_EXECUTABLE}"
            --update-interval-ms "${INVALID_INTERVAL}"
        RESULT_VARIABLE RESULT
        OUTPUT_VARIABLE OUTPUT
        ERROR_VARIABLE ERROR
    )
    if (RESULT EQUAL 0)
        message(FATAL_ERROR
            "Benchmark accepted invalid cadence ${INVALID_INTERVAL}")
    endif()
    if (NOT ERROR MATCHES
        "Invalid application-like update interval: ${INVALID_INTERVAL}")
        message(FATAL_ERROR
            "Benchmark did not identify invalid cadence ${INVALID_INTERVAL}: ${ERROR}")
    endif()
    if (OUTPUT MATCHES
        "cadence_mode=application_like|representative_time_to_visible=true")
        message(FATAL_ERROR
            "Invalid cadence ${INVALID_INTERVAL} emitted representative metadata")
    endif()
endforeach()
