if (NOT DEFINED BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "BENCHMARK_EXECUTABLE is required")
endif()

function(assert_evidence_contract MODE OUTPUT_VARIABLE EXPECTED_SCOPE EXCLUDED_SCOPE)
    set(BENCHMARK_OUTPUT "${${OUTPUT_VARIABLE}}")
    if (NOT BENCHMARK_OUTPUT MATCHES
        "configuration [^\n]*evidence_scope=${EXPECTED_SCOPE} shipped_time_to_visible_evidence=false interactive_time_to_visible_evidence=false comparison_budget_ms=100000\\.000 comparison_budget_role=operator_supplied")
        message(FATAL_ERROR
            "${MODE} output omitted required evidence or comparison-budget labels")
    endif()
    if (BENCHMARK_OUTPUT MATCHES "evidence_scope=${EXCLUDED_SCOPE}")
        message(FATAL_ERROR "${MODE} output mixed evidence scopes")
    endif()
    if (NOT BENCHMARK_OUTPUT MATCHES
        "assessment comparison_budget_status=within[^\n]*comparison_budget_ms=100000\\.000 comparison_budget_role=operator_supplied_comparison_only assessment_scope=numeric_result_only")
        message(FATAL_ERROR
            "${MODE} budget assessment did not classify only the numeric result")
    endif()
    if (BENCHMARK_OUTPUT MATCHES
        "neighbor[-_a-z]*policy|policy_conclusion")
        message(FATAL_ERROR
            "${MODE} budget assessment emitted a neighbor-policy conclusion")
    endif()
endfunction()

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
    "configuration [^\n]*mesh_submission_limit_source=runtime_diagnostics mesh_submission_behavior=effective_bounded effective_mesh_submission_limit=1[^\n]*cadence_mode=application_like update_interval_ms=16\\.667")
    message(FATAL_ERROR
        "Default application-like cadence metadata was absent or incorrect")
endif()
assert_evidence_contract(
    "Default application-like"
    APPLICATION_OUTPUT
    controlled_fixture_application_like_cadence
    nonrepresentative_scheduler_lower_bound)
string(REGEX MATCHALL "sample index=[^\n]*" APPLICATION_SAMPLE_LINES
    "${APPLICATION_OUTPUT}")
list(LENGTH APPLICATION_SAMPLE_LINES APPLICATION_SAMPLE_COUNT)
if (NOT APPLICATION_SAMPLE_COUNT EQUAL 4)
    message(FATAL_ERROR
        "Application-like benchmark emitted ${APPLICATION_SAMPLE_COUNT} samples instead of 4")
endif()
foreach(WORKLOAD IN ITEMS stationary positive_x positive_z diagonal_xz)
    if (NOT APPLICATION_OUTPUT MATCHES
        "sample index=0 workload=${WORKLOAD} [^\n]*desired_to_generation_start_ms=[0-9]+\\.[0-9]+ [^\n]*generation_queue_wait_ms=[0-9]+\\.[0-9]+ [^\n]*generation_execution_ms=[0-9]+\\.[0-9]+ [^\n]*data_ready_to_neighbors_ready_ms=[0-9]+\\.[0-9]+ [^\n]*neighbors_ready_to_mesh_start_ms=[0-9]+\\.[0-9]+ [^\n]*mesh_execution_ms=[0-9]+\\.[0-9]+ [^\n]*desired_to_accepted_geometry_ms=[0-9]+\\.[0-9]+ [^\n]*desired_to_first_draw_ms=unavailable")
        message(FATAL_ERROR
            "Application-like benchmark omitted required ${WORKLOAD} stages")
    endif()
    if (NOT APPLICATION_OUTPUT MATCHES
        "cohort workload=${WORKLOAD} [^\n]*desired_to_generation_start_p99_ms=[0-9]+\\.[0-9]+ [^\n]*generation_queue_wait_p99_ms=[0-9]+\\.[0-9]+ [^\n]*desired_to_accepted_geometry_p99_ms=[0-9]+\\.[0-9]+ [^\n]*desired_to_first_draw_samples=0")
        message(FATAL_ERROR
            "Application-like benchmark omitted ${WORKLOAD} P99 or draw limitation")
    endif()
endforeach()
string(REGEX MATCHALL "sample index=[^\n]*completion_state=quiescent"
    APPLICATION_QUIESCENT_SAMPLES "${APPLICATION_OUTPUT}")
list(LENGTH APPLICATION_QUIESCENT_SAMPLES APPLICATION_QUIESCENT_COUNT)
if (NOT APPLICATION_QUIESCENT_COUNT EQUAL APPLICATION_SAMPLE_COUNT)
    message(FATAL_ERROR
        "Application-like benchmark accepted a sample before Quiescent")
endif()
foreach(SAMPLE_LINE IN LISTS APPLICATION_SAMPLE_LINES)
    if (NOT SAMPLE_LINE MATCHES
        "generation_started=([0-9]+) generation_completed=([0-9]+) generation_cancelled=([0-9]+) generation_failed=([0-9]+)")
        message(FATAL_ERROR
            "Application-like benchmark omitted a generation accounting partition")
    endif()
    set(GENERATION_STARTED "${CMAKE_MATCH_1}")
    set(GENERATION_COMPLETED "${CMAKE_MATCH_2}")
    set(GENERATION_CANCELLED "${CMAKE_MATCH_3}")
    set(GENERATION_FAILED "${CMAKE_MATCH_4}")
    math(EXPR GENERATION_SETTLED
        "${GENERATION_COMPLETED} + ${GENERATION_CANCELLED}")
    if (NOT GENERATION_STARTED EQUAL GENERATION_SETTLED OR
        GENERATION_FAILED GREATER GENERATION_COMPLETED)
        message(FATAL_ERROR
            "Application-like benchmark emitted an invalid generation accounting partition: ${SAMPLE_LINE}")
    endif()

    if (NOT SAMPLE_LINE MATCHES
        "mesh_started=([0-9]+) mesh_completed=([0-9]+) mesh_accepted=([0-9]+) mesh_stale=([0-9]+) mesh_failed=([0-9]+)")
        message(FATAL_ERROR
            "Application-like benchmark omitted a mesh accounting partition")
    endif()
    set(MESH_STARTED "${CMAKE_MATCH_1}")
    set(MESH_COMPLETED "${CMAKE_MATCH_2}")
    set(MESH_ACCEPTED "${CMAKE_MATCH_3}")
    set(MESH_STALE "${CMAKE_MATCH_4}")
    set(MESH_FAILED "${CMAKE_MATCH_5}")
    math(EXPR MESH_TERMINAL
        "${MESH_ACCEPTED} + ${MESH_STALE} + ${MESH_FAILED}")
    if (NOT MESH_STARTED EQUAL MESH_COMPLETED OR
        NOT MESH_COMPLETED EQUAL MESH_TERMINAL)
        message(FATAL_ERROR
            "Application-like benchmark emitted an invalid mesh accounting partition: ${SAMPLE_LINE}")
    endif()
endforeach()
string(REGEX MATCHALL
    "sample index=[^\n]*generation_pending=0 generation_in_flight=0 generation_completion_pending=0 generation_terminal_failures=0 canonical_source_work=0 canonical_generation_work=0 canonical_retired_work=0[^\n]*mesh_pending=0 mesh_in_flight=0 mesh_completion_pending=0 mesh_terminal_failures=0 chunk_load_pending=0 chunk_load_in_flight=0 chunk_load_terminal_failures=0 eviction_pending=0 eviction_in_flight=0 eviction_terminal_failures=0[^\n]*completion_state=quiescent"
    APPLICATION_ZERO_OWNER_SAMPLES "${APPLICATION_OUTPUT}")
list(LENGTH APPLICATION_ZERO_OWNER_SAMPLES APPLICATION_ZERO_OWNER_COUNT)
if (NOT APPLICATION_ZERO_OWNER_COUNT EQUAL APPLICATION_SAMPLE_COUNT)
    message(FATAL_ERROR
        "Application-like benchmark omitted exact zero owner gauges")
endif()
if (NOT APPLICATION_OUTPUT MATCHES
    "data_ready_to_neighbors_ready_boundary=(inferred_data_ready|observed_final_neighbor)")
    message(FATAL_ERROR
        "Application-like benchmark omitted neighbor-boundary provenance")
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

function(assert_rejected OPTION VALUE EXPECTED_ERROR)
    execute_process(
        COMMAND "${BENCHMARK_EXECUTABLE}" "${OPTION}" "${VALUE}"
        RESULT_VARIABLE REJECTED_RESULT
        OUTPUT_VARIABLE REJECTED_OUTPUT
        ERROR_VARIABLE REJECTED_ERROR
    )
    if (REJECTED_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Benchmark accepted ${OPTION} ${VALUE}")
    endif()
    if (NOT REJECTED_ERROR MATCHES "${EXPECTED_ERROR}")
        message(FATAL_ERROR
            "Benchmark misdiagnosed ${OPTION} ${VALUE}: ${REJECTED_ERROR}")
    endif()
    if (REJECTED_OUTPUT MATCHES "benchmark name=|configuration ")
        message(FATAL_ERROR
            "Rejected ${OPTION} ${VALUE} emitted benchmark evidence")
    endif()
endfunction()

assert_rejected(--samples 1001 "Invalid sample count")
assert_rejected(--samples 1x "Invalid integer")
assert_rejected(--samples 9223372036854775808 "Invalid integer")
assert_rejected(--view-distance 17 "Unsupported view distance")
assert_rejected(--worker-threads 65 "Unsupported worker count")
assert_rejected(--mesh-queue-limit 32769 "Unsupported mesh queue limit")
assert_rejected(--motion-steps 100001 "Unsafe motion step count")
assert_rejected(--timeout-seconds 3601 "Invalid timeout")
assert_rejected(--comparison-budget-ms 3600001 "Invalid positive duration")

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
    "configuration [^\n]*cadence_mode=scheduler_lower_bound_stress update_interval_ms=unpaced evidence_scope=nonrepresentative_scheduler_lower_bound shipped_time_to_visible_evidence=false interactive_time_to_visible_evidence=false")
    message(FATAL_ERROR
        "Scheduler lower-bound stress metadata was absent or incorrect")
endif()
assert_evidence_contract(
    "Scheduler lower-bound stress"
    STRESS_OUTPUT
    nonrepresentative_scheduler_lower_bound
    controlled_fixture_application_like_cadence)
string(REGEX MATCHALL "sample index=[^\n]*" STRESS_SAMPLE_LINES
    "${STRESS_OUTPUT}")
list(LENGTH STRESS_SAMPLE_LINES STRESS_SAMPLE_COUNT)
if (NOT STRESS_SAMPLE_COUNT EQUAL 4)
    message(FATAL_ERROR
        "Scheduler stress benchmark emitted ${STRESS_SAMPLE_COUNT} samples instead of 4")
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
        COMMAND "${BENCHMARK_EXECUTABLE}"
            --samples 1
            --timeout-seconds 1
            ${MIXED_CADENCE_ARGUMENTS}
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
        "cadence_mode=|evidence_scope=")
        message(FATAL_ERROR
            "Rejected mixed cadence modes emitted evidence metadata")
    endif()
endforeach()

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}"
        --samples 1
        --view-distance 1
        --worker-threads 4
        --mesh-queue-limit 1
        --timeout-seconds 10
        --comparison-budget-ms 100000
        --update-interval-ms 5
        --workload stationary
    RESULT_VARIABLE EXPLICIT_RESULT
    OUTPUT_VARIABLE EXPLICIT_OUTPUT
    ERROR_VARIABLE EXPLICIT_ERROR
)
if (NOT EXPLICIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Explicit application-like benchmark failed: ${EXPLICIT_ERROR}")
endif()
if (NOT EXPLICIT_OUTPUT MATCHES
    "configuration [^\n]*worker_threads=4[^\n]*mesh_queue_limit_setting=1 mesh_submission_limit_source=runtime_diagnostics mesh_submission_behavior=effective_bounded effective_mesh_submission_limit=1[^\n]*cadence_mode=application_like update_interval_ms=5\\.000")
    message(FATAL_ERROR
        "Explicit application-like cadence or scheduler metadata was absent or incorrect")
endif()
assert_evidence_contract(
    "Explicit application-like"
    EXPLICIT_OUTPUT
    controlled_fixture_application_like_cadence
    nonrepresentative_scheduler_lower_bound)
string(REGEX MATCHALL "sample index=[^\n]*completion_state=quiescent"
    EXPLICIT_QUIESCENT_SAMPLES "${EXPLICIT_OUTPUT}")
list(LENGTH EXPLICIT_QUIESCENT_SAMPLES EXPLICIT_QUIESCENT_COUNT)
if (NOT EXPLICIT_QUIESCENT_COUNT EQUAL 1)
    message(FATAL_ERROR
        "Explicit application-like cadence did not produce one Quiescent sample")
endif()

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}"
        --samples 1
        --view-distance 1
        --worker-threads 2
        --timeout-seconds 10
        --comparison-budget-ms 100000
        --workload stationary
        --collect-debug-detail
    RESULT_VARIABLE DETAIL_RESULT
    OUTPUT_VARIABLE DETAIL_OUTPUT
    ERROR_VARIABLE DETAIL_ERROR
)
if (NOT DETAIL_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Opt-in debug-detail benchmark failed: ${DETAIL_ERROR}")
endif()
if (NOT DETAIL_OUTPUT MATCHES
    "configuration [^\n]*debug_detail_collection=per_update_opt_in")
    message(FATAL_ERROR
        "Opt-in debug-detail benchmark omitted its instrumentation mode")
endif()
if (NOT DETAIL_OUTPUT MATCHES
    "sample index=0 workload=stationary [^\n]*debug_detail_snapshots=[1-9][0-9]* debug_detail_records=[1-9][0-9]*")
    message(FATAL_ERROR
        "Opt-in debug-detail benchmark did not collect bounded snapshots")
endif()
foreach(INVALID_INTERVAL IN ITEMS
        0.0000001
        9223372036854.775
        1e300)
    execute_process(
        COMMAND "${BENCHMARK_EXECUTABLE}"
            --samples 1
            --timeout-seconds 1
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
        "cadence_mode=application_like|evidence_scope=")
        message(FATAL_ERROR
            "Invalid cadence ${INVALID_INTERVAL} emitted representative metadata")
    endif()
endforeach()
