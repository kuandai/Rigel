#include "BlockTargetingDetail.h"

#include "Rigel/Voxel/BlockModel.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef RIGEL_BENCHMARK_BUILD_TYPE
#define RIGEL_BENCHMARK_BUILD_TYPE "unknown"
#endif

#ifndef RIGEL_BENCHMARK_COMPILER
#define RIGEL_BENCHMARK_COMPILER "unknown"
#endif

#ifndef RIGEL_BENCHMARK_SYSTEM
#define RIGEL_BENCHMARK_SYSTEM "unknown"
#endif

#ifndef RIGEL_BENCHMARK_PROCESSOR
#define RIGEL_BENCHMARK_PROCESSOR "unknown"
#endif

namespace {
std::atomic<uint64_t> allocationCount = 0;

void* countedAllocation(std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(std::max<std::size_t>(size, 1))) {
        return memory;
    }
    throw std::bad_alloc();
}

void* countedAlignedAllocation(std::size_t size, std::size_t alignment) {
    void* memory = nullptr;
    if (posix_memalign(
            &memory, alignment, std::max<std::size_t>(size, 1)) != 0) {
        throw std::bad_alloc();
    }
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    return memory;
}
} // namespace

void* operator new(std::size_t size) {
    return countedAllocation(size);
}

void* operator new[](std::size_t size) {
    return countedAllocation(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return countedAlignedAllocation(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return countedAlignedAllocation(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

namespace {
using namespace Rigel::Voxel;
using BenchmarkClock = std::chrono::steady_clock;

constexpr size_t kDefaultIterations = 20;
constexpr size_t kMaximumIterations = 100;
constexpr size_t kDefaultWarmupIterations = 3;
constexpr size_t kMaximumWarmupIterations = 20;
constexpr size_t kDefaultRaysPerIteration = 256;
constexpr size_t kMaximumRaysPerIteration = 8192;

struct Options {
    size_t iterations = kDefaultIterations;
    size_t warmupIterations = kDefaultWarmupIterations;
    size_t raysPerIteration = kDefaultRaysPerIteration;
    bool validateOnly = false;
};

enum class ParseResult {
    Run,
    Help,
    Error,
};

struct ExpectedHit {
    glm::ivec3 block;
    BlockID id;
    float distance;
    Direction face;
    size_t cuboidIndex;
};

struct RayCase {
    glm::vec3 origin;
    glm::vec3 direction;
    float maxDistance;
    std::optional<ExpectedHit> expected;
};

struct Workload {
    std::string_view name;
    World* world = nullptr;
    std::vector<RayCase> cases;
    size_t nonAirBlocks = 0;
    std::string_view layout;
};

struct WorkloadValidation {
    detail::BlockRaycastCounters counters;
    size_t expectedHits = 0;
};

struct Measurement {
    std::vector<double> nanosecondsPerRay;
    uint64_t resultChecksum = 0;
    uint64_t allocations = 0;
};

BlockModelFace inventedFace() {
    return BlockModelFace{.textureSlot = "invented_surface"};
}

BlockModelCuboid inventedCuboid(
    BlockModelBounds bounds,
    std::initializer_list<Direction> faces = {
        Direction::PosX, Direction::NegX,
        Direction::PosY, Direction::NegY,
        Direction::PosZ, Direction::NegZ}
) {
    BlockModelCuboid cuboid;
    cuboid.bounds = bounds;
    for (const Direction face : faces) {
        cuboid.faces[static_cast<size_t>(face)] = inventedFace();
    }
    return cuboid;
}

std::shared_ptr<const BlockModel> inventedModel(
    std::string identifier,
    std::vector<BlockModelCuboid> cuboids
) {
    return std::make_shared<const BlockModel>(
        std::move(identifier),
        std::vector<std::string>{"invented_surface"},
        std::move(cuboids));
}

class BenchmarkFixture {
public:
    BenchmarkFixture()
        : emptyWorld(resources)
        , denseWorld(resources)
        , mixedWorld(resources)
        , galleryWorld(resources)
        , longRangeWorld(resources) {
        registerTypes();
        populateWorlds();
        resources.registry().freeze();
    }

    std::vector<Workload> workloads() {
        return {
            makeEmptyLongWorkload(),
            makeDenseCubeWorkload(),
            makeMixedWorkload(),
            makeGalleryWorkload(),
            makeLongRangeWorkload(),
        };
    }

    const BlockRegistry& registry() const {
        return resources.registry();
    }

    BlockID cubeId() const {
        return cube;
    }

private:
    BlockID registerModel(
        std::string identifier,
        std::shared_ptr<const BlockModel> model
    ) {
        BlockType type;
        type.model = std::move(model);
        return resources.registry().registerBlock(
            std::move(identifier), std::move(type));
    }

    void registerTypes() {
        cube = registerModel(
            "benchmark:canonical_cube", BlockModel::fullCube());
        slab = registerModel(
            "benchmark:slab",
            inventedModel(
                "benchmark:model/slab",
                {inventedCuboid(
                    {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}})}));
        post = registerModel(
            "benchmark:post",
            inventedModel(
                "benchmark:model/post",
                {inventedCuboid(
                    {{0.25f, 0.0f, 0.25f},
                     {0.75f, 1.0f, 0.75f}})}));
        stair = registerModel(
            "benchmark:stair",
            inventedModel(
                "benchmark:model/stair",
                {
                    inventedCuboid(
                        {{0.0f, 0.0f, 0.0f},
                         {1.0f, 0.5f, 1.0f}}),
                    inventedCuboid(
                        {{0.5f, 0.5f, 0.0f},
                         {1.0f, 1.0f, 1.0f}}),
                }));
        crossedPlanes = registerModel(
            "benchmark:crossed_planes",
            inventedModel(
                "benchmark:model/crossed_planes",
                {
                    inventedCuboid(
                        {{0.5f, 0.0f, 0.0f},
                         {0.5f, 1.0f, 1.0f}},
                        {Direction::PosX, Direction::NegX}),
                    inventedCuboid(
                        {{0.0f, 0.0f, 0.5f},
                         {1.0f, 1.0f, 0.5f}},
                        {Direction::PosZ, Direction::NegZ}),
                }));
        overhang = registerModel(
            "benchmark:overhang",
            inventedModel(
                "benchmark:model/overhang",
                {inventedCuboid(
                    {{-0.25f, -0.25f, -0.25f},
                     {1.25f, 1.25f, 1.25f}})}));
    }

    void populateWorlds() {
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                for (int z = 0; z < 8; ++z) {
                    denseWorld.setBlock(x, y, z, BlockState{cube});
                }
            }
        }

        mixedWorld.setBlock(0, 0, 0, BlockState{slab});
        mixedWorld.setBlock(0, 0, 2, BlockState{post});
        mixedWorld.setBlock(0, 0, 4, BlockState{stair});
        mixedWorld.setBlock(0, 0, 6, BlockState{crossedPlanes});
        mixedWorld.setBlock(0, 0, 8, BlockState{overhang});

        constexpr std::array<size_t, 5> galleryPattern = {0, 1, 2, 3, 4};
        const std::array<BlockID, 5> galleryTypes = {
            cube, slab, post, stair, overhang};
        for (int row = 0; row < 16; ++row) {
            for (int column = 0; column < 16; ++column) {
                const size_t pattern = galleryPattern[
                    static_cast<size_t>(row * 16 + column) %
                    galleryPattern.size()];
                galleryWorld.setBlock(
                    column * 4, 1, row * 4,
                    BlockState{galleryTypes[pattern]});
            }
        }

        longRangeWorld.setBlock(384, 0, 0, BlockState{cube});
    }

    Workload makeEmptyLongWorkload() {
        return {
            "empty_long",
            &emptyWorld,
            {{{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 128.0f,
              std::nullopt}},
            0,
            "empty_128_cell_segment",
        };
    }

    Workload makeDenseCubeWorkload() {
        Workload workload{
            "dense_full_cubes", &denseWorld, {}, 512, "8x8x8_solid"};
        workload.cases.reserve(64);
        for (int y = 0; y < 8; ++y) {
            for (int z = 0; z < 8; ++z) {
                workload.cases.push_back({
                    {-0.5f, static_cast<float>(y) + 0.5f,
                     static_cast<float>(z) + 0.5f},
                    {1.0f, 0.0f, 0.0f},
                    16.0f,
                    ExpectedHit{
                        {0, y, z}, cube, 0.5f,
                        Direction::NegX, 0},
                });
            }
        }
        return workload;
    }

    Workload makeMixedWorkload() {
        return {
            "mixed_partial_models",
            &mixedWorld,
            {
                {{-1.0f, 0.25f, 0.5f}, {1.0f, 0.0f, 0.0f}, 4.0f,
                 ExpectedHit{{0, 0, 0}, slab, 1.0f, Direction::NegX, 0}},
                {{-1.0f, 0.75f, 2.5f}, {1.0f, 0.0f, 0.0f}, 4.0f,
                 ExpectedHit{{0, 0, 2}, post, 1.25f, Direction::NegX, 0}},
                {{-1.0f, 0.75f, 4.5f}, {1.0f, 0.0f, 0.0f}, 4.0f,
                 ExpectedHit{{0, 0, 4}, stair, 1.5f, Direction::NegX, 1}},
                {{-1.0f, 0.5f, 6.25f}, {1.0f, 0.0f, 0.0f}, 4.0f,
                 ExpectedHit{
                     {0, 0, 6}, crossedPlanes, 1.5f,
                     Direction::NegX, 0}},
                {{-1.0f, 0.5f, 8.5f}, {1.0f, 0.0f, 0.0f}, 4.0f,
                 ExpectedHit{
                     {0, 0, 8}, overhang, 0.75f,
                     Direction::NegX, 0}},
            },
            5,
            "five_isolated_partial_models",
        };
    }

    Workload makeGalleryWorkload() {
        Workload workload{
            "gallery_like_density",
            &galleryWorld,
            {},
            256,
            "16x16_spacing4",
        };
        const std::array<BlockID, 5> ids = {
            cube, slab, post, stair, overhang};
        const std::array<float, 5> distances = {
            2.0f, 2.5f, 2.0f, 2.0f, 1.75f};
        const std::array<size_t, 5> cuboids = {0, 0, 0, 1, 0};
        workload.cases.reserve(256);
        for (int row = 0; row < 16; ++row) {
            for (int column = 0; column < 16; ++column) {
                const size_t pattern = static_cast<size_t>(row * 16 + column)
                    % ids.size();
                workload.cases.push_back({
                    {static_cast<float>(column * 4) + 0.5f,
                     4.0f,
                     static_cast<float>(row * 4) + 0.5f},
                    {0.0f, -1.0f, 0.0f},
                    4.0f,
                    ExpectedHit{
                        {column * 4, 1, row * 4},
                        ids[pattern], distances[pattern],
                        Direction::PosY, cuboids[pattern]},
                });
            }
        }
        return workload;
    }

    Workload makeLongRangeWorkload() {
        return {
            "long_range_target",
            &longRangeWorld,
            {{{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 512.0f,
              ExpectedHit{
                  {384, 0, 0}, cube, 383.5f,
                  Direction::NegX, 0}}},
            1,
            "cube_at_384_cells",
        };
    }

    WorldResources resources;
    World emptyWorld;
    World denseWorld;
    World mixedWorld;
    World galleryWorld;
    World longRangeWorld;
    BlockID cube;
    BlockID slab;
    BlockID post;
    BlockID stair;
    BlockID crossedPlanes;
    BlockID overhang;
};

void addCounters(
    detail::BlockRaycastCounters& total,
    const detail::BlockRaycastCounters& addition
) {
    total.ddaCellsVisited += addition.ddaCellsVisited;
    total.ownerCandidateSlots += addition.ownerCandidateSlots;
    total.ownerCandidateRetestsAvoided +=
        addition.ownerCandidateRetestsAvoided;
    total.ownersTested += addition.ownersTested;
    total.nonAirOwnersTested += addition.nonAirOwnersTested;
    total.canonicalCubeTests += addition.canonicalCubeTests;
    total.cuboidsTested += addition.cuboidsTested;
    total.declaredFacesTested += addition.declaredFacesTested;
}

void validateHit(
    const Workload& workload,
    const RayCase& ray,
    const std::optional<BlockTarget>& target
) {
    if (!ray.expected) {
        if (target) {
            throw std::runtime_error(
                std::string(workload.name) + " unexpectedly hit a block");
        }
        return;
    }
    if (!target) {
        throw std::runtime_error(
            std::string(workload.name) + " omitted an expected hit");
    }
    const ExpectedHit& expected = *ray.expected;
    if (target->block != expected.block || target->state.id != expected.id ||
        target->face != expected.face ||
        target->cuboidIndex != expected.cuboidIndex ||
        std::abs(target->distance - expected.distance) > 0.00001f) {
        throw std::runtime_error(
            std::string(workload.name) + " returned different hit geometry");
    }
}

WorkloadValidation validateWorkload(const Workload& workload) {
    WorkloadValidation validation;
    for (const RayCase& ray : workload.cases) {
        detail::BlockRaycastCounters counters;
        const auto instrumented = detail::raycastBlockWithCounters(
            *workload.world, ray.origin, ray.direction,
            ray.maxDistance, counters);
        const auto normal = raycastBlock(
            *workload.world, ray.origin, ray.direction, ray.maxDistance);
        validateHit(workload, ray, instrumented);
        if (instrumented != normal) {
            throw std::runtime_error(
                std::string(workload.name) +
                " instrumented and production paths diverged");
        }
        addCounters(validation.counters, counters);
        validation.expectedHits += ray.expected ? 1 : 0;
    }
    const auto& counters = validation.counters;
    if (counters.ownerCandidateSlots !=
        counters.ownerCandidateRetestsAvoided + counters.ownersTested) {
        throw std::runtime_error(
            std::string(workload.name) +
            " candidate accounting is incomplete");
    }

    const uint64_t allocationsBefore =
        allocationCount.load(std::memory_order_relaxed);
    for (const RayCase& ray : workload.cases) {
        const auto target = raycastBlock(
            *workload.world, ray.origin, ray.direction, ray.maxDistance);
        validateHit(workload, ray, target);
    }
    const uint64_t allocationsAfter =
        allocationCount.load(std::memory_order_relaxed);
    if (allocationsAfter != allocationsBefore) {
        throw std::runtime_error(
            std::string(workload.name) + " raycast allocated heap memory");
    }
    return validation;
}

uint64_t targetChecksum(const std::optional<BlockTarget>& target) {
    if (!target) {
        return UINT64_C(0x6a09e667f3bcc909);
    }
    uint64_t result = target->state.id.type;
    result = result * 1315423911u + static_cast<uint32_t>(target->block.x);
    result = result * 1315423911u + static_cast<uint32_t>(target->block.y);
    result = result * 1315423911u + static_cast<uint32_t>(target->block.z);
    result ^= static_cast<uint64_t>(std::bit_cast<uint32_t>(target->distance))
        << 16;
    result ^= static_cast<uint64_t>(target->cuboidIndex) << 8;
    result ^= static_cast<uint64_t>(target->face);
    return result;
}

uint64_t runRays(const Workload& workload, size_t rayCount) {
    uint64_t checksum = 0;
    size_t caseIndex = 0;
    for (size_t rayIndex = 0; rayIndex < rayCount; ++rayIndex) {
        const RayCase& ray = workload.cases[caseIndex];
        checksum += targetChecksum(raycastBlock(
            *workload.world, ray.origin, ray.direction, ray.maxDistance));
        if (++caseIndex == workload.cases.size()) {
            caseIndex = 0;
        }
    }
    return checksum;
}

Measurement measure(
    const Workload& workload,
    size_t warmupIterations,
    size_t iterations,
    size_t raysPerIteration
) {
    uint64_t warmupGuard = 0;
    for (size_t iteration = 0; iteration < warmupIterations; ++iteration) {
        warmupGuard ^= runRays(workload, raysPerIteration) + iteration;
    }

    Measurement measurement;
    measurement.nanosecondsPerRay.reserve(iterations);
    measurement.resultChecksum = warmupGuard;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const uint64_t allocationsBefore =
            allocationCount.load(std::memory_order_relaxed);
        const auto start = BenchmarkClock::now();
        const uint64_t checksum = runRays(workload, raysPerIteration);
        const auto end = BenchmarkClock::now();
        const uint64_t allocationsAfter =
            allocationCount.load(std::memory_order_relaxed);
        measurement.nanosecondsPerRay.push_back(
            std::chrono::duration<double, std::nano>(end - start).count() /
            static_cast<double>(raysPerIteration));
        measurement.allocations += allocationsAfter - allocationsBefore;
        measurement.resultChecksum ^=
            checksum + iteration * UINT64_C(0x9e3779b97f4a7c15);
    }
    if (measurement.allocations != 0) {
        throw std::runtime_error(
            std::string(workload.name) + " timed raycast allocated memory");
    }
    return measurement;
}

double nearestRank(std::vector<double> values, double percentile) {
    std::sort(values.begin(), values.end());
    const size_t rank = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(percentile * values.size())));
    return values[rank - 1];
}

void printValidation(
    const Workload& workload,
    const WorkloadValidation& validation
) {
    const auto& counters = validation.counters;
    const float maximumRayDistance = std::max_element(
        workload.cases.begin(), workload.cases.end(),
        [](const RayCase& left, const RayCase& right) {
            return left.maxDistance < right.maxDistance;
        })->maxDistance;
    std::cout
        << "validation workload=" << workload.name
        << " status=passed"
        << " ray_cases=" << workload.cases.size()
        << " expected_hits=" << validation.expectedHits
        << " non_air_blocks=" << workload.nonAirBlocks
        << " layout=" << workload.layout
        << " maximum_ray_distance=" << maximumRayDistance
        << " dda_cells=" << counters.ddaCellsVisited
        << " candidate_slots=" << counters.ownerCandidateSlots
        << " candidate_retests_avoided="
        << counters.ownerCandidateRetestsAvoided
        << " candidate_retests_executed=0"
        << " owners_tested=" << counters.ownersTested
        << " non_air_owners_tested=" << counters.nonAirOwnersTested
        << " canonical_cube_tests=" << counters.canonicalCubeTests
        << " cuboids_tested=" << counters.cuboidsTested
        << " declared_faces_tested=" << counters.declaredFacesTested
        << " hot_path_allocations=0\n";
}

double printMeasurement(
    const Workload& workload,
    const Measurement& measurement,
    size_t raysPerIteration
) {
    const auto [minimum, maximum] = std::minmax_element(
        measurement.nanosecondsPerRay.begin(),
        measurement.nanosecondsPerRay.end());
    const double total = std::accumulate(
        measurement.nanosecondsPerRay.begin(),
        measurement.nanosecondsPerRay.end(), 0.0);
    const double mean = total / measurement.nanosecondsPerRay.size();
    std::cout
        << "workload name=" << workload.name
        << " iterations=" << measurement.nanosecondsPerRay.size()
        << " rays_per_iteration=" << raysPerIteration
        << " total_rays="
        << measurement.nanosecondsPerRay.size() * raysPerIteration
        << " mean_ns_per_ray=" << mean
        << " min_ns_per_ray=" << *minimum
        << " p50_ns_per_ray="
        << nearestRank(measurement.nanosecondsPerRay, 0.50)
        << " p95_ns_per_ray="
        << nearestRank(measurement.nanosecondsPerRay, 0.95)
        << " max_ns_per_ray=" << *maximum
        << " hot_path_allocations=" << measurement.allocations
        << " result_checksum=" << measurement.resultChecksum
        << '\n';
    return mean;
}

std::optional<size_t> parseBounded(
    std::string_view value,
    size_t minimum,
    size_t maximum
) {
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum ||
        parsed > std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<size_t>(parsed);
}

void printUsage() {
    std::cout
        << "Usage: Rigel_block_targeting_benchmark [options]\n"
        << "  --iterations N  timed samples per workload (1-"
        << kMaximumIterations << ", default " << kDefaultIterations << ")\n"
        << "  --warmup N      untimed samples per workload (0-"
        << kMaximumWarmupIterations << ", default "
        << kDefaultWarmupIterations << ")\n"
        << "  --rays N        raycasts per sample (1-"
        << kMaximumRaysPerIteration << ", default "
        << kDefaultRaysPerIteration << ")\n"
        << "  --validate-only validate every fixture without timing\n";
}

ParseResult parseOptions(int argc, char** argv, Options& options) {
    bool iterationsSpecified = false;
    bool warmupSpecified = false;
    bool raysSpecified = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            printUsage();
            return ParseResult::Help;
        }
        if (argument == "--validate-only") {
            if (options.validateOnly) {
                std::cerr << "Validation mode may be specified only once\n";
                return ParseResult::Error;
            }
            options.validateOnly = true;
            continue;
        }
        if (argument != "--iterations" && argument != "--warmup" &&
            argument != "--rays") {
            std::cerr << "Unknown option: " << argument << '\n';
            return ParseResult::Error;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return ParseResult::Error;
        }
        const std::string_view value(argv[++index]);
        bool* specified = nullptr;
        size_t* destination = nullptr;
        size_t minimum = 0;
        size_t maximum = 0;
        std::string_view label;
        if (argument == "--iterations") {
            specified = &iterationsSpecified;
            destination = &options.iterations;
            minimum = 1;
            maximum = kMaximumIterations;
            label = "iteration";
        } else if (argument == "--warmup") {
            specified = &warmupSpecified;
            destination = &options.warmupIterations;
            maximum = kMaximumWarmupIterations;
            label = "warmup";
        } else {
            specified = &raysSpecified;
            destination = &options.raysPerIteration;
            minimum = 1;
            maximum = kMaximumRaysPerIteration;
            label = "ray";
        }
        if (*specified) {
            std::cerr << label << " count may be specified only once\n";
            return ParseResult::Error;
        }
        const auto parsed = parseBounded(value, minimum, maximum);
        if (!parsed) {
            std::cerr << "Invalid bounded " << label << " count: " << value
                      << " (expected " << minimum << '-' << maximum << ")\n";
            return ParseResult::Error;
        }
        *destination = *parsed;
        *specified = true;
    }
    return ParseResult::Run;
}

int runBenchmark(int argc, char** argv) {
    Options options;
    const ParseResult parseResult = parseOptions(argc, argv, options);
    if (parseResult != ParseResult::Run) {
        return parseResult == ParseResult::Help ? 0 : 2;
    }

    BenchmarkFixture fixture;
    std::vector<Workload> workloads = fixture.workloads();
    std::vector<WorkloadValidation> validations;
    validations.reserve(workloads.size());
    for (const Workload& workload : workloads) {
        validations.push_back(validateWorkload(workload));
    }

    const auto& extents = fixture.registry().modelExtents();
    if (!fixture.registry().frozen() || !extents ||
        extents->min != std::array{-0.25f, -0.25f, -0.25f} ||
        extents->max != std::array{1.25f, 1.25f, 1.25f} ||
        fixture.registry().getType(fixture.cubeId()).model.geometry.get() !=
            BlockModel::fullCube().get()) {
        throw std::runtime_error("benchmark registry contract changed");
    }

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "benchmark name=block_targeting version=1"
        << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
        << " intended_build=Release"
        << " compiler=" << RIGEL_BENCHMARK_COMPILER
        << " system=" << RIGEL_BENCHMARK_SYSTEM
        << " processor=" << RIGEL_BENCHMARK_PROCESSOR
        << " hardware_threads=" << std::thread::hardware_concurrency()
        << " clock=steady_clock"
        << " clock_is_steady=" << (BenchmarkClock::is_steady ? "true" : "false")
        << " fixture=deterministic_invented_models"
        << " production_configuration=false\n";
    std::cout
        << "validation status=passed"
        << " output_checked_before_timing=true"
        << " workload_count=" << workloads.size()
        << " registry_frozen=true"
        << " registry_types=" << fixture.registry().size()
        << " canonical_cube_fast_path=true"
        << " aggregate_extent_min=-0.25,-0.25,-0.25"
        << " aggregate_extent_max=1.25,1.25,1.25"
        << " owner_candidate_dimensions=3x3x3"
        << " maximum_owner_candidates_per_cell=27"
        << " hot_path_allocation_probe=global_new_replacement\n";
    for (size_t index = 0; index < workloads.size(); ++index) {
        printValidation(workloads[index], validations[index]);
    }

    if (options.validateOnly) {
        std::cout << "configuration mode=validate_only\n";
        return 0;
    }

    std::cout
        << "configuration mode=timed"
        << " iterations_per_workload=" << options.iterations
        << " warmup_iterations_per_workload=" << options.warmupIterations
        << " rays_per_iteration=" << options.raysPerIteration
        << " maximum_iterations=" << kMaximumIterations
        << " maximum_warmup_iterations=" << kMaximumWarmupIterations
        << " maximum_rays_per_iteration=" << kMaximumRaysPerIteration
        << " percentile_method=nearest_rank"
        << " workload_order=empty_long,dense_full_cubes,"
           "mixed_partial_models,gallery_like_density,long_range_target\n";

    double slowestMean = 0.0;
    uint64_t resultGuard = 0;
    for (const Workload& workload : workloads) {
        const Measurement measurement = measure(
            workload, options.warmupIterations,
            options.iterations, options.raysPerIteration);
        resultGuard ^= measurement.resultChecksum;
        slowestMean = std::max(
            slowestMean,
            printMeasurement(
                workload, measurement, options.raysPerIteration));
    }
    constexpr double frameBudgetNanoseconds = 1'000'000'000.0 / 60.0;
    std::cout
        << "suitability context=one_center_ray_per_frame"
        << " frame_rate_hz=60"
        << " frame_budget_ns=" << frameBudgetNanoseconds
        << " slowest_mean_ns_per_ray=" << slowestMean
        << " slowest_mean_frame_fraction="
        << slowestMean / frameBudgetNanoseconds
        << " benchmark_overhead_included=true\n";

    static volatile uint64_t benchmarkResultGuard = 0;
    benchmarkResultGuard = resultGuard;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return runBenchmark(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "block_targeting_benchmark_failed reason=\""
                  << error.what() << "\"\n";
    } catch (...) {
        std::cerr
            << "block_targeting_benchmark_failed reason=unknown_exception\n";
    }
    return 1;
}
