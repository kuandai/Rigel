if (NOT DEFINED BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "BENCHMARK_EXECUTABLE is required")
endif()

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}" --iterations 1 --warmup 0
    RESULT_VARIABLE TIMED_RESULT
    OUTPUT_VARIABLE TIMED_OUTPUT
    ERROR_VARIABLE TIMED_ERROR
)
if (NOT TIMED_RESULT EQUAL 0)
    message(FATAL_ERROR "Entity collision benchmark failed: ${TIMED_ERROR}")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "benchmark name=entity_collision version=1[^\n]*fixture=deterministic_synthetic_blocks production_configuration=false")
    message(FATAL_ERROR "Entity collision benchmark omitted environment metadata")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "validation status=passed fixture_count=5 canonical_full_cube=true immutable_partial_box_spans=true output_checked_before_timing=true allocation_counter=global_operator_new_calls")
    message(FATAL_ERROR "Entity collision benchmark omitted pre-timing validation")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "configuration mode=timed iterations_per_workload=1 warmup_iterations_per_workload=0 maximum_iterations=20000 maximum_warmup_iterations=2000 movement_axes_per_entity=1 collision_queries_per_entity=2 axis_order=x,y,z allocation_timing_scope=workload_body")
    message(FATAL_ERROR "Entity collision benchmark omitted bounded run metadata")
endif()

foreach(EXPECTED_LINE IN ITEMS
    "workload name=empty_world_movement shape_context=air_only iterations=1 entities_per_iteration=1 entity_updates=1 broadphase_candidates_per_iteration=90 validated_boxes_per_sweep=0"
    "workload name=dense_full_cube_collision shape_context=filled_32x32x32_canonical_full_cube iterations=1 entities_per_iteration=1 entity_updates=1 broadphase_candidates_per_iteration=108 validated_boxes_per_sweep=8"
    "workload name=mixed_partial_shapes shape_context=filled_32x32x32_alternating_1_and_2_box_shapes iterations=1 entities_per_iteration=1 entity_updates=1 broadphase_candidates_per_iteration=108 validated_boxes_per_sweep=11"
    "workload name=high_speed_sweep shape_context=single_full_cube_at_cell_200 iterations=1 entities_per_iteration=1 entity_updates=1 broadphase_candidates_per_iteration=2349 validated_boxes_per_sweep=1"
    "workload name=multiple_independent_entities shape_context=64_entities_against_independent_full_cube_cells iterations=1 entities_per_iteration=64 entity_updates=64 broadphase_candidates_per_iteration=5184 validated_boxes_per_sweep=64"
)
    if (NOT TIMED_OUTPUT MATCHES
        "${EXPECTED_LINE} elapsed_ms=[0-9]+\.[0-9]+ ns_per_entity_update=[0-9]+\.[0-9]+ relative_total_cost_to_empty=[0-9]+\.[0-9]+ relative_per_entity_cost_to_empty=[0-9]+\.[0-9]+ allocations=[0-9]+ allocations_per_iteration=[0-9]+\.[0-9]+ allocation_owner=[a-z_]+ result_checksum=[0-9]+")
        message(FATAL_ERROR
            "Entity collision benchmark omitted workload evidence: ${EXPECTED_LINE}\n${TIMED_OUTPUT}")
    endif()
endforeach()

foreach(ALLOCATION_FREE_WORKLOAD IN ITEMS
    empty_world_movement
    dense_full_cube_collision
    mixed_partial_shapes
    high_speed_sweep
)
    if (NOT TIMED_OUTPUT MATCHES
        "workload name=${ALLOCATION_FREE_WORKLOAD}[^\n]* allocations=0 allocations_per_iteration=0.000 allocation_owner=none")
        message(FATAL_ERROR
            "${ALLOCATION_FREE_WORKLOAD} allocated in the collision path")
    endif()
endforeach()
if (NOT TIMED_OUTPUT MATCHES
    "workload name=multiple_independent_entities[^\n]* allocations=1 allocations_per_iteration=1.000 allocation_owner=world_entities_iteration_snapshot")
    message(FATAL_ERROR "Multiple-entity allocation ownership was not reported")
endif()

string(FIND "${TIMED_OUTPUT}" "validation status=passed" VALIDATION_POSITION)
string(FIND "${TIMED_OUTPUT}" "workload name=empty_world_movement" TIMING_POSITION)
if (VALIDATION_POSITION LESS 0 OR TIMING_POSITION LESS 0 OR
    NOT VALIDATION_POSITION LESS TIMING_POSITION)
    message(FATAL_ERROR "Fixture validation did not precede timed evidence")
endif()

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}" --validate-only
    RESULT_VARIABLE VALIDATE_RESULT
    OUTPUT_VARIABLE VALIDATE_OUTPUT
    ERROR_VARIABLE VALIDATE_ERROR
)
if (NOT VALIDATE_RESULT EQUAL 0 OR
    NOT VALIDATE_OUTPUT MATCHES "configuration mode=validate_only" OR
    VALIDATE_OUTPUT MATCHES "workload name=")
    message(FATAL_ERROR
        "Validate-only mode violated its contract: ${VALIDATE_ERROR}")
endif()

function(assert_rejected EXPECTED_ERROR)
    execute_process(
        COMMAND "${BENCHMARK_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE RESULT
        OUTPUT_VARIABLE OUTPUT
        ERROR_VARIABLE ERROR
    )
    if (RESULT EQUAL 0)
        message(FATAL_ERROR "Benchmark accepted invalid arguments: ${ARGN}")
    endif()
    if (NOT ERROR MATCHES "${EXPECTED_ERROR}")
        message(FATAL_ERROR "Benchmark misdiagnosed '${ARGN}': ${ERROR}")
    endif()
    if (OUTPUT MATCHES "benchmark name=|validation status=|workload name=")
        message(FATAL_ERROR "Rejected arguments emitted benchmark evidence: ${ARGN}")
    endif()
endfunction()

assert_rejected("Invalid bounded iteration count" --iterations 0)
assert_rejected("Invalid bounded iteration count" --iterations 20001)
assert_rejected("Invalid bounded iteration count" --iterations 1x)
assert_rejected("Invalid bounded iteration count"
    --iterations 18446744073709551616)
assert_rejected("Invalid bounded warmup count" --warmup 2001)
assert_rejected("Iteration count may be specified only once"
    --iterations 1 --iterations 2)
assert_rejected("Missing value for --warmup" --warmup)
assert_rejected("Unknown option" --workload dense_full_cube_collision)
