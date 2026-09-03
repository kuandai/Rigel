if (NOT DEFINED BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "BENCHMARK_EXECUTABLE is required")
endif()

execute_process(
    COMMAND "${BENCHMARK_EXECUTABLE}"
        --iterations 1 --warmup 0 --rays 1
    RESULT_VARIABLE TIMED_RESULT
    OUTPUT_VARIABLE TIMED_OUTPUT
    ERROR_VARIABLE TIMED_ERROR
)
if (NOT TIMED_RESULT EQUAL 0)
    message(FATAL_ERROR "Targeting benchmark failed: ${TIMED_ERROR}")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "benchmark name=block_targeting version=1[^\n]*intended_build=Release[^\n]*fixture=deterministic_invented_models production_configuration=false")
    message(FATAL_ERROR "Targeting benchmark omitted environment metadata")
endif()
if (NOT TIMED_OUTPUT MATCHES
    "validation status=passed output_checked_before_timing=true workload_count=5 registry_frozen=true registry_types=7 canonical_cube_fast_path=true aggregate_extent_min=-0.25,-0.25,-0.25 aggregate_extent_max=1.25,1.25,1.25 owner_candidate_dimensions=3x3x3 maximum_owner_candidates_per_cell=27 hot_path_allocation_probe=global_new_replacement")
    message(FATAL_ERROR "Targeting benchmark omitted pre-timing validation")
endif()

foreach(EXPECTED_VALIDATION IN ITEMS
    "empty_long status=passed ray_cases=1 expected_hits=0 non_air_blocks=0 layout=empty_128_cell_segment maximum_ray_distance=128.000 dda_cells=129 candidate_slots=3483 candidate_retests_avoided=2304 candidate_retests_executed=0 owners_tested=1179 non_air_owners_tested=0 canonical_cube_tests=0 cuboids_tested=0 declared_faces_tested=0 hot_path_allocations=0"
    "dense_full_cubes status=passed ray_cases=64 expected_hits=64 non_air_blocks=512 layout=8x8x8_solid maximum_ray_distance=16.000 dda_cells=128 candidate_slots=3456 candidate_retests_avoided=1152 candidate_retests_executed=0 owners_tested=2304 non_air_owners_tested=968 canonical_cube_tests=968 cuboids_tested=0 declared_faces_tested=0 hot_path_allocations=0"
    "mixed_partial_models status=passed ray_cases=5 expected_hits=5 non_air_blocks=5 layout=five_isolated_partial_models maximum_ray_distance=4.000 dda_cells=9 candidate_slots=243 candidate_retests_avoided=72 candidate_retests_executed=0 owners_tested=171 non_air_owners_tested=5 canonical_cube_tests=0 cuboids_tested=7 declared_faces_tested=34 hot_path_allocations=0"
    "gallery_like_density status=passed ray_cases=256 expected_hits=256 non_air_blocks=256 layout=16x16_spacing4 maximum_ray_distance=4.000 dda_cells=973 candidate_slots=26271 candidate_retests_avoided=12906 candidate_retests_executed=0 owners_tested=13365 non_air_owners_tested=256 canonical_cube_tests=52 cuboids_tested=255 declared_faces_tested=1530 hot_path_allocations=0"
    "long_range_target status=passed ray_cases=1 expected_hits=1 non_air_blocks=1 layout=cube_at_384_cells maximum_ray_distance=512.000 dda_cells=385 candidate_slots=10395 candidate_retests_avoided=6912 candidate_retests_executed=0 owners_tested=3483 non_air_owners_tested=1 canonical_cube_tests=1 cuboids_tested=0 declared_faces_tested=0 hot_path_allocations=0"
)
    if (NOT TIMED_OUTPUT MATCHES
        "validation workload=${EXPECTED_VALIDATION}")
        message(FATAL_ERROR
            "Targeting benchmark omitted candidate context: ${EXPECTED_VALIDATION}")
    endif()
endforeach()

if (NOT TIMED_OUTPUT MATCHES
    "configuration mode=timed iterations_per_workload=1 warmup_iterations_per_workload=0 rays_per_iteration=1 maximum_iterations=100 maximum_warmup_iterations=20 maximum_rays_per_iteration=8192 percentile_method=nearest_rank workload_order=empty_long,dense_full_cubes,mixed_partial_models,gallery_like_density,long_range_target")
    message(FATAL_ERROR "Targeting benchmark omitted bounded run metadata")
endif()
foreach(WORKLOAD IN ITEMS
    empty_long
    dense_full_cubes
    mixed_partial_models
    gallery_like_density
    long_range_target
)
    if (NOT TIMED_OUTPUT MATCHES
        "workload name=${WORKLOAD} iterations=1 rays_per_iteration=1 total_rays=1 mean_ns_per_ray=[0-9]+\\.[0-9]+ min_ns_per_ray=[0-9]+\\.[0-9]+ p50_ns_per_ray=[0-9]+\\.[0-9]+ p95_ns_per_ray=[0-9]+\\.[0-9]+ max_ns_per_ray=[0-9]+\\.[0-9]+ hot_path_allocations=0 result_checksum=[0-9]+")
        message(FATAL_ERROR
            "Targeting benchmark omitted timing evidence: ${WORKLOAD}")
    endif()
endforeach()
if (NOT TIMED_OUTPUT MATCHES
    "suitability context=one_center_ray_per_frame frame_rate_hz=60 frame_budget_ns=[0-9]+\\.[0-9]+ slowest_mean_ns_per_ray=[0-9]+\\.[0-9]+ slowest_mean_frame_fraction=[0-9]+\\.[0-9]+ benchmark_overhead_included=true")
    message(FATAL_ERROR "Targeting benchmark omitted frame context")
endif()

string(FIND "${TIMED_OUTPUT}" "validation status=passed" VALIDATION_POSITION)
string(FIND "${TIMED_OUTPUT}" "workload name=empty_long" TIMING_POSITION)
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
assert_rejected("Invalid bounded warmup count" --warmup 21)
assert_rejected("Invalid bounded ray count" --rays 0)
assert_rejected("Invalid bounded ray count" --rays 8193)
assert_rejected("Invalid bounded ray count" --rays 1x)
assert_rejected("ray count may be specified only once" --rays 1 --rays 2)
assert_rejected("Missing value for --warmup" --warmup)
assert_rejected("Unknown option" --workload empty_long)
