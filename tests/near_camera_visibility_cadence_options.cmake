if (NOT DEFINED BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "BENCHMARK_EXECUTABLE is required")
endif()

set(SAMPLE_ARGUMENTS
    --samples 1
    --view-distance 1
    --worker-threads 2
    --timeout-seconds 10
    --comparison-budget-ms 100000
)

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}" ${SAMPLE_ARGUMENTS}
    RESULT_VARIABLE APPLICATION_RESULT
    OUTPUT_VARIABLE APPLICATION_OUTPUT
    ERROR_VARIABLE APPLICATION_ERROR
)
if (NOT APPLICATION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Default application-like benchmark failed: ${APPLICATION_ERROR}")
endif()
if (NOT APPLICATION_OUTPUT MATCHES
    "benchmark name=near_camera_visibility[^\n]*wait_signal=streaming_quiescent")
    message(FATAL_ERROR
        "Application-like benchmark did not declare its lifecycle wait signal")
endif()
if (NOT APPLICATION_OUTPUT MATCHES
    "configuration [^\n]*cadence_mode=application_like update_interval_ms=16\\.667 representative_time_to_visible=true")
    message(FATAL_ERROR
        "Default application-like cadence metadata was absent or incorrect")
endif()
string(REGEX MATCHALL "sample index=[^\n]*" APPLICATION_SAMPLE_LINES
    "${APPLICATION_OUTPUT}")
list(LENGTH APPLICATION_SAMPLE_LINES APPLICATION_SAMPLE_COUNT)
if (NOT APPLICATION_SAMPLE_COUNT EQUAL 2)
    message(FATAL_ERROR
        "Application-like benchmark emitted ${APPLICATION_SAMPLE_COUNT} samples instead of 2")
endif()
string(REGEX MATCHALL "sample index=[^\n]*completion_state=quiescent"
    APPLICATION_QUIESCENT_SAMPLES "${APPLICATION_OUTPUT}")
list(LENGTH APPLICATION_QUIESCENT_SAMPLES APPLICATION_QUIESCENT_COUNT)
if (NOT APPLICATION_QUIESCENT_COUNT EQUAL APPLICATION_SAMPLE_COUNT)
    message(FATAL_ERROR
        "Application-like benchmark accepted a sample before Quiescent")
endif()
string(REGEX MATCHALL "lifecycle_updates=[0-9]+" APPLICATION_UPDATE_FIELDS
    "${APPLICATION_OUTPUT}")
list(LENGTH APPLICATION_UPDATE_FIELDS APPLICATION_UPDATE_FIELD_COUNT)
if (NOT APPLICATION_UPDATE_FIELD_COUNT EQUAL APPLICATION_SAMPLE_COUNT)
    message(FATAL_ERROR
        "Application-like benchmark omitted lifecycle update accounting")
endif()
set(APPLICATION_UPDATE_TOTAL 0)
foreach(UPDATE_FIELD IN LISTS APPLICATION_UPDATE_FIELDS)
    string(REGEX REPLACE "lifecycle_updates=([0-9]+)" "\\1"
        UPDATE_COUNT "${UPDATE_FIELD}")
    if (UPDATE_COUNT GREATER 601)
        message(FATAL_ERROR
            "Default cadence ran ${UPDATE_COUNT} updates within a 10-second sample; expected at most 601 paced updates")
    endif()
    math(EXPR APPLICATION_UPDATE_TOTAL
        "${APPLICATION_UPDATE_TOTAL} + ${UPDATE_COUNT}")
endforeach()

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}" ${SAMPLE_ARGUMENTS}
        --scheduler-lower-bound-stress
    RESULT_VARIABLE STRESS_RESULT
    OUTPUT_VARIABLE STRESS_OUTPUT
    ERROR_VARIABLE STRESS_ERROR
)
if (NOT STRESS_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Scheduler lower-bound stress benchmark failed: ${STRESS_ERROR}")
endif()
if (NOT STRESS_OUTPUT MATCHES
    "benchmark name=near_camera_visibility[^\n]*wait_signal=streaming_quiescent")
    message(FATAL_ERROR
        "Scheduler stress benchmark did not declare its lifecycle wait signal")
endif()
if (NOT STRESS_OUTPUT MATCHES
    "configuration [^\n]*cadence_mode=scheduler_lower_bound_stress update_interval_ms=unpaced representative_time_to_visible=false")
    message(FATAL_ERROR
        "Scheduler lower-bound stress metadata was absent or incorrect")
endif()
string(REGEX MATCHALL "sample index=[^\n]*" STRESS_SAMPLE_LINES
    "${STRESS_OUTPUT}")
list(LENGTH STRESS_SAMPLE_LINES STRESS_SAMPLE_COUNT)
if (NOT STRESS_SAMPLE_COUNT EQUAL 2)
    message(FATAL_ERROR
        "Scheduler stress benchmark emitted ${STRESS_SAMPLE_COUNT} samples instead of 2")
endif()
string(REGEX MATCHALL "sample index=[^\n]*completion_state=quiescent"
    STRESS_QUIESCENT_SAMPLES "${STRESS_OUTPUT}")
list(LENGTH STRESS_QUIESCENT_SAMPLES STRESS_QUIESCENT_COUNT)
if (NOT STRESS_QUIESCENT_COUNT EQUAL STRESS_SAMPLE_COUNT)
    message(FATAL_ERROR
        "Scheduler stress benchmark accepted a sample before Quiescent")
endif()
string(REGEX MATCHALL "lifecycle_updates=[0-9]+" STRESS_UPDATE_FIELDS
    "${STRESS_OUTPUT}")
list(LENGTH STRESS_UPDATE_FIELDS STRESS_UPDATE_FIELD_COUNT)
if (NOT STRESS_UPDATE_FIELD_COUNT EQUAL STRESS_SAMPLE_COUNT)
    message(FATAL_ERROR
        "Scheduler stress benchmark omitted lifecycle update accounting")
endif()
set(STRESS_UPDATE_TOTAL 0)
foreach(UPDATE_FIELD IN LISTS STRESS_UPDATE_FIELDS)
    string(REGEX REPLACE "lifecycle_updates=([0-9]+)" "\\1"
        UPDATE_COUNT "${UPDATE_FIELD}")
    math(EXPR STRESS_UPDATE_TOTAL "${STRESS_UPDATE_TOTAL} + ${UPDATE_COUNT}")
endforeach()
math(EXPR MINIMUM_STRESS_UPDATE_TOTAL "${APPLICATION_UPDATE_TOTAL} * 2")
if (NOT STRESS_UPDATE_TOTAL GREATER MINIMUM_STRESS_UPDATE_TOTAL)
    message(FATAL_ERROR
        "Scheduler stress did not exercise an observably unpaced loop: ${STRESS_UPDATE_TOTAL} updates versus ${APPLICATION_UPDATE_TOTAL} paced updates")
endif()

foreach(STRESS_FIRST IN ITEMS FALSE TRUE)
    if (STRESS_FIRST)
        set(MIXED_CADENCE_ARGUMENTS
            --scheduler-lower-bound-stress --update-interval-ms 5)
    else()
        set(MIXED_CADENCE_ARGUMENTS
            --update-interval-ms 5 --scheduler-lower-bound-stress)
    endif()
    execute_process(
        COMMAND "${BENCHMARK_EXECUTABLE}" ${MIXED_CADENCE_ARGUMENTS}
        RESULT_VARIABLE MIXED_CADENCE_RESULT
        OUTPUT_VARIABLE MIXED_CADENCE_OUTPUT
        ERROR_VARIABLE MIXED_CADENCE_ERROR
    )
    if (MIXED_CADENCE_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Benchmark accepted an explicit cadence combined with scheduler stress")
    endif()
    if (NOT MIXED_CADENCE_ERROR MATCHES "cannot be combined")
        message(FATAL_ERROR
            "Benchmark did not identify mutually exclusive cadence modes: ${MIXED_CADENCE_ERROR}")
    endif()
    if (MIXED_CADENCE_OUTPUT MATCHES
        "cadence_mode=|representative_time_to_visible=")
        message(FATAL_ERROR
            "Rejected mixed cadence modes emitted evidence metadata")
    endif()
endforeach()

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
