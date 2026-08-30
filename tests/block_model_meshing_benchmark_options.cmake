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
    message(FATAL_ERROR "Meshing benchmark failed: ${TIMED_ERROR}")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "benchmark name=block_model_meshing version=1[^\n]*texture_atlas_entries=2 texture_atlas_upload=cpu_only fixture=deterministic_invented_cuboids production_configuration=false")
    message(FATAL_ERROR "Meshing benchmark omitted environment metadata")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "validation status=passed canonical_cube_fast_path=true registry_frozen=true fixture_count=3 output_checked_before_timing=true")
    message(FATAL_ERROR "Meshing benchmark omitted pre-timing validation")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "configuration mode=timed iterations_per_workload=1 warmup_iterations_per_workload=0 maximum_iterations=100 maximum_warmup_iterations=20 percentile_method=nearest_rank workload_order=all_cube,representative_mixed,non_cube_heavy")
    message(FATAL_ERROR "Meshing benchmark omitted bounded run metadata")
endif()

foreach(EXPECTED_LINE IN ITEMS
    "workload name=all_cube non_air_blocks=16384 canonical_cube_blocks=16384 single_cuboid_blocks=0 multiple_cuboid_blocks=0 vertices=16384 indices=24576 iterations=1"
    "workload name=representative_mixed non_air_blocks=16384 canonical_cube_blocks=12288 single_cuboid_blocks=2048 multiple_cuboid_blocks=2048 vertices=189440 indices=284160 iterations=1"
    "workload name=non_cube_heavy non_air_blocks=16384 canonical_cube_blocks=0 single_cuboid_blocks=10240 multiple_cuboid_blocks=6144 vertices=540672 indices=811008 iterations=1"
)
    if (NOT TIMED_OUTPUT MATCHES
        "${EXPECTED_LINE} total_ms=[0-9]+\\.[0-9]+ mean_ms=[0-9]+\\.[0-9]+ min_ms=[0-9]+\\.[0-9]+ p50_ms=[0-9]+\\.[0-9]+ p95_ms=[0-9]+\\.[0-9]+ max_ms=[0-9]+\\.[0-9]+ result_checksum=[0-9]+")
        message(FATAL_ERROR
            "Meshing benchmark omitted workload evidence: ${EXPECTED_LINE}")
    endif()
endforeach()

string(FIND "${TIMED_OUTPUT}" "validation status=passed" VALIDATION_POSITION)
string(FIND "${TIMED_OUTPUT}" "workload name=all_cube" TIMING_POSITION)
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
        message(FATAL_ERROR
            "Benchmark misdiagnosed '${ARGN}': ${ERROR}")
    endif()
    if (OUTPUT MATCHES "benchmark name=|validation status=|workload name=")
        message(FATAL_ERROR
            "Rejected arguments emitted benchmark evidence: ${ARGN}")
    endif()
endfunction()

assert_rejected("Invalid bounded iteration count" --iterations 0)
assert_rejected("Invalid bounded iteration count" --iterations 101)
assert_rejected("Invalid bounded iteration count" --iterations 1x)
assert_rejected("Invalid bounded iteration count"
    --iterations 18446744073709551616)
assert_rejected("Invalid bounded warmup count" --warmup 21)
assert_rejected("Iteration count may be specified only once"
    --iterations 1 --iterations 2)
assert_rejected("Missing value for --warmup" --warmup)
assert_rejected("Unknown option" --workload all_cube)
