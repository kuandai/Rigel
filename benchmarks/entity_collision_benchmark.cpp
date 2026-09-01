#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

thread_local bool countAllocations = false;
thread_local uint64_t allocationCount = 0;

void recordAllocation() noexcept {
    if (countAllocations) {
        ++allocationCount;
    }
}

void* allocate(size_t size) {
    if (void* memory = std::malloc(std::max<size_t>(size, 1))) {
        recordAllocation();
        return memory;
    }
    throw std::bad_alloc();
}

void* allocateAligned(size_t size, size_t alignment) {
    const size_t nonzeroSize = std::max<size_t>(size, 1);
    if (nonzeroSize > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        throw std::bad_alloc();
    }
    const size_t roundedSize =
        ((nonzeroSize + alignment - 1) / alignment) * alignment;
    if (void* memory = std::aligned_alloc(alignment, roundedSize)) {
        recordAllocation();
        return memory;
    }
    throw std::bad_alloc();
}

} // namespace

void* operator new(size_t size) {
    return allocate(size);
}

void* operator new[](size_t size) {
    return allocate(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(size_t size, std::align_val_t alignment) {
    return allocateAligned(size, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment) {
    return allocateAligned(size, static_cast<size_t>(alignment));
}

void* operator new(
    size_t size,
    std::align_val_t alignment,
    const std::nothrow_t&
) noexcept {
    try {
        return allocateAligned(size, static_cast<size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    size_t size,
    std::align_val_t alignment,
    const std::nothrow_t&
) noexcept {
    try {
        return allocateAligned(size, static_cast<size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, size_t, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, size_t, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete(
    void* memory,
    std::align_val_t,
    const std::nothrow_t&
) noexcept {
    std::free(memory);
}

void operator delete[](
    void* memory,
    std::align_val_t,
    const std::nothrow_t&
) noexcept {
    std::free(memory);
}

namespace {

using namespace Rigel;
using BenchmarkClock = std::chrono::steady_clock;

constexpr size_t kDefaultIterations = 5'000;
constexpr size_t kDefaultWarmupIterations = 200;
constexpr size_t kMaximumIterations = 20'000;
constexpr size_t kMaximumWarmupIterations = 2'000;
constexpr size_t kMultipleEntityCount = 64;
constexpr float kEntityHalfExtent = 0.25f;

struct Options {
    size_t iterations = kDefaultIterations;
    size_t warmupIterations = kDefaultWarmupIterations;
    bool validateOnly = false;
};

enum class ParseResult {
    Run,
    Help,
    Error,
};

enum class WorkloadKind : uint8_t {
    EmptyWorld,
    DenseFullCube,
    MixedPartial,
    HighSpeedSweep,
    MultipleEntities,
};

struct WorkloadContext {
    WorkloadKind kind;
    std::string_view name;
    std::string_view shapeContext;
    size_t entitiesPerIteration;
    uint64_t broadphaseCandidatesPerIteration;
    size_t validatedBoxesPerSweep;
    std::string_view allocationOwner;
};

struct Measurement {
    double elapsedMilliseconds = 0.0;
    uint64_t allocations = 0;
    uint64_t checksum = 0;
};

class GravityFreeEntity final : public Entity::Entity {
public:
    GravityFreeEntity() : Entity("benchmark:collision_entity") {
        m_gravityModifier = 0.0f;
        setLocalBounds(Rigel::Entity::Aabb{
            glm::vec3(-kEntityHalfExtent),
            glm::vec3(kEntityHalfExtent),
        });
    }
};

Voxel::BlockID registerShape(
    Voxel::WorldResources& resources,
    std::string_view identifier,
    Voxel::BlockCollisionShape collision
) {
    const std::string ownedIdentifier(identifier);
    Voxel::BlockType type;
    type.identifier = ownedIdentifier;
    type.collision = std::move(collision);
    return resources.registry().registerBlock(
        ownedIdentifier, std::move(type));
}

Voxel::BlockCollisionBox sweptBounds(float startX, float delta, float z) {
    Voxel::BlockCollisionBox bounds{
        .min = {startX - kEntityHalfExtent,
                0.5f - kEntityHalfExtent,
                z - kEntityHalfExtent},
        .max = {startX + kEntityHalfExtent,
                0.5f + kEntityHalfExtent,
                z + kEntityHalfExtent},
    };
    if (delta > 0.0f) {
        bounds.max[0] += delta;
    } else {
        bounds.min[0] += delta;
    }
    return bounds;
}

uint64_t broadphaseCandidateCount(
    const Voxel::BlockCollisionBox& bounds
) {
    uint64_t candidates = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        const double minimum = std::ceil(
            static_cast<double>(bounds.min[axis]) -
            Voxel::BlockCollisionShape::MaximumCoordinate -
            Voxel::BlockCollisionContactTolerance);
        const double maximum = std::floor(
            static_cast<double>(bounds.max[axis]) -
            Voxel::BlockCollisionShape::MinimumCoordinate +
            Voxel::BlockCollisionContactTolerance);
        candidates *= static_cast<uint64_t>(maximum - minimum + 1.0);
    }
    return candidates;
}

Voxel::BlockCollisionBox supportBounds(float finalX, float z) {
    Voxel::BlockCollisionBox bounds{
        .min = {finalX - kEntityHalfExtent,
                0.5f - kEntityHalfExtent,
                z - kEntityHalfExtent},
        .max = {finalX + kEntityHalfExtent,
                0.5f + kEntityHalfExtent,
                z + kEntityHalfExtent},
    };
    bounds.min[1] -= Voxel::BlockCollisionContactTolerance * 2.0f;
    return bounds;
}

size_t overlappingBoxes(
    const Voxel::World& world,
    const Voxel::BlockCollisionBox& bounds
) {
    size_t count = 0;
    if (!world.forEachCollisionBox(bounds, [&](const auto&) { ++count; })) {
        throw std::runtime_error("benchmark collision query was rejected");
    }
    return count;
}

bool near(float actual, float expected) {
    return std::abs(actual - expected) <= 0.00001f;
}

class CollisionFixture final {
public:
    CollisionFixture()
        : m_emptyWorld(m_resources)
        , m_fullWorld(m_resources)
        , m_mixedWorld(m_resources)
        , m_highSpeedWorld(m_resources)
        , m_multipleWorld(m_resources) {
        m_fullCubeId = registerShape(
            m_resources,
            "benchmark:full_cube",
            Voxel::BlockCollisionShape::fullCube());
        m_singlePartialId = registerShape(
            m_resources,
            "benchmark:single_partial",
            Voxel::BlockCollisionShape::boxes({
                {{0.25f, 0.0f, 0.0f}, {0.5f, 1.0f, 1.0f}},
            }));
        m_multiplePartialId = registerShape(
            m_resources,
            "benchmark:multiple_partial",
            Voxel::BlockCollisionShape::boxes({
                {{0.125f, 0.0f, 0.0f}, {0.25f, 1.0f, 1.0f}},
                {{0.625f, 0.0f, 0.0f}, {0.875f, 1.0f, 1.0f}},
            }));
        m_resources.registry().freeze();

        m_fullWorld.chunkManager().getOrCreateChunk({0, 0, 0}).fill(
            Voxel::BlockState{m_fullCubeId}, m_resources.registry());

        std::vector<Voxel::BlockState> mixedBlocks(Voxel::Chunk::VOLUME);
        for (int z = 0; z < Voxel::Chunk::SIZE; ++z) {
            for (int y = 0; y < Voxel::Chunk::SIZE; ++y) {
                for (int x = 0; x < Voxel::Chunk::SIZE; ++x) {
                    const size_t index = static_cast<size_t>(
                        x + y * Voxel::Chunk::SIZE +
                        z * Voxel::Chunk::SIZE * Voxel::Chunk::SIZE);
                    mixedBlocks[index] = Voxel::BlockState{
                        ((x + y + z) & 1) == 0
                            ? m_singlePartialId
                            : m_multiplePartialId,
                    };
                }
            }
        }
        m_mixedWorld.chunkManager().getOrCreateChunk({0, 0, 0}).copyFrom(
            mixedBlocks, m_resources.registry());

        m_highSpeedWorld.setBlock(
            200, 0, 0, Voxel::BlockState{m_fullCubeId});
        for (size_t index = 0; index < kMultipleEntityCount; ++index) {
            m_multipleWorld.setBlock(
                3, 0, static_cast<int>(index),
                Voxel::BlockState{m_fullCubeId});
            auto entity = std::make_unique<GravityFreeEntity>();
            m_multipleEntities.push_back(entity.get());
            if (m_multipleWorld.entities().spawn(std::move(entity)).isNull()) {
                throw std::runtime_error("failed to spawn benchmark entity");
            }
        }
    }

    std::array<WorkloadContext, 5> validate() {
        const auto& registry = m_resources.registry();
        if (!registry.frozen() ||
            !registry.getType(m_fullCubeId).collision.isFullCube() ||
            registry.getType(m_singlePartialId).collision.boxes().size() != 1 ||
            registry.getType(m_multiplePartialId).collision.boxes().size() != 2) {
            throw std::runtime_error("collision shape fixture is not canonical");
        }

        validateOne(WorkloadKind::EmptyWorld, m_emptyEntity, 4.5f, false);
        validateOne(
            WorkloadKind::DenseFullCube,
            m_fullEntity,
            -kEntityHalfExtent - Voxel::BlockCollisionContactTolerance,
            true);
        validateOne(
            WorkloadKind::MixedPartial,
            m_mixedEntity,
            -Voxel::BlockCollisionContactTolerance,
            true);
        validateOne(
            WorkloadKind::HighSpeedSweep,
            m_highSpeedEntity,
            200.0f - kEntityHalfExtent -
                Voxel::BlockCollisionContactTolerance,
            true);

        prepareMultipleEntities();
        m_multipleWorld.tickEntities(0.5f);
        for (const GravityFreeEntity* entity : m_multipleEntities) {
            if (!entity->collidedX() ||
                !near(
                    entity->position().x,
                    3.0f - kEntityHalfExtent -
                        Voxel::BlockCollisionContactTolerance)) {
                throw std::runtime_error(
                    "multiple-entity output validation failed");
            }
        }

        const auto context = workloadContexts();
        const std::array<size_t, 5> expectedBoxes{0, 8, 11, 1, 64};
        for (size_t index = 0; index < context.size(); ++index) {
            if (context[index].validatedBoxesPerSweep != expectedBoxes[index]) {
                throw std::runtime_error(
                    "collision query context validation failed for " +
                    std::string(context[index].name) + ": expected " +
                    std::to_string(expectedBoxes[index]) + ", observed " +
                    std::to_string(context[index].validatedBoxesPerSweep));
            }
        }
        return context;
    }

    uint64_t run(WorkloadKind kind) {
        switch (kind) {
            case WorkloadKind::EmptyWorld:
                prepare(m_emptyEntity, 0.5f, 8.0f);
                m_emptyEntity.update(m_emptyWorld, 0.5f);
                return checksum(m_emptyEntity);
            case WorkloadKind::DenseFullCube:
                prepare(m_fullEntity, -1.0f, 8.0f);
                m_fullEntity.update(m_fullWorld, 1.0f);
                return checksum(m_fullEntity);
            case WorkloadKind::MixedPartial:
                prepare(m_mixedEntity, -1.0f, 8.0f);
                m_mixedEntity.update(m_mixedWorld, 1.0f);
                return checksum(m_mixedEntity);
            case WorkloadKind::HighSpeedSweep:
                prepare(m_highSpeedEntity, 0.5f, 256.0f);
                m_highSpeedEntity.update(m_highSpeedWorld, 1.0f);
                return checksum(m_highSpeedEntity);
            case WorkloadKind::MultipleEntities: {
                prepareMultipleEntities();
                m_multipleWorld.tickEntities(0.5f);
                uint64_t result = 0;
                for (const GravityFreeEntity* entity : m_multipleEntities) {
                    result += checksum(*entity);
                }
                return result;
            }
        }
        return 0;
    }

private:
    static void prepare(
        GravityFreeEntity& entity,
        float startX,
        float velocityX,
        float z = 0.5f
    ) {
        entity.setPosition(startX, 0.5f, z);
        entity.setVelocity(glm::vec3(velocityX, 0.0f, 0.0f));
    }

    void prepareMultipleEntities() {
        for (size_t index = 0; index < m_multipleEntities.size(); ++index) {
            prepare(
                *m_multipleEntities[index],
                0.5f,
                8.0f,
                static_cast<float>(index) + 0.5f);
        }
    }

    static uint64_t checksum(const GravityFreeEntity& entity) {
        const int64_t quantized = static_cast<int64_t>(
            std::llround(static_cast<double>(entity.position().x) * 1'000'000.0));
        return static_cast<uint64_t>(quantized) ^
            (entity.collidedX() ? UINT64_C(0x9e3779b97f4a7c15) : 0);
    }

    void validateOne(
        WorkloadKind kind,
        GravityFreeEntity& entity,
        float expectedX,
        bool expectedCollision
    ) {
        run(kind);
        if (!near(entity.position().x, expectedX) ||
            entity.collidedX() != expectedCollision) {
            throw std::runtime_error("entity collision output validation failed");
        }
    }

    std::array<WorkloadContext, 5> workloadContexts() const {
        const auto makeContext = [](
            WorkloadKind kind,
            std::string_view name,
            std::string_view shapeContext,
            const Voxel::World& world,
            float startX,
            float delta,
            float finalX,
            size_t entities,
            std::string_view allocationOwner
        ) {
            const auto sweep = sweptBounds(startX, delta, 0.5f);
            const uint64_t candidates =
                broadphaseCandidateCount(sweep) +
                broadphaseCandidateCount(supportBounds(finalX, 0.5f));
            return WorkloadContext{
                .kind = kind,
                .name = name,
                .shapeContext = shapeContext,
                .entitiesPerIteration = entities,
                .broadphaseCandidatesPerIteration = candidates * entities,
                .validatedBoxesPerSweep =
                    overlappingBoxes(world, sweep) * entities,
                .allocationOwner = allocationOwner,
            };
        };

        return {{
            makeContext(
                WorkloadKind::EmptyWorld,
                "empty_world_movement",
                "air_only",
                m_emptyWorld,
                0.5f, 4.0f, 4.5f, 1, "none"),
            makeContext(
                WorkloadKind::DenseFullCube,
                "dense_full_cube_collision",
                "filled_32x32x32_canonical_full_cube",
                m_fullWorld,
                -1.0f, 8.0f,
                -kEntityHalfExtent -
                    Voxel::BlockCollisionContactTolerance,
                1, "none"),
            makeContext(
                WorkloadKind::MixedPartial,
                "mixed_partial_shapes",
                "filled_32x32x32_alternating_1_and_2_box_shapes",
                m_mixedWorld,
                -1.0f, 8.0f,
                -Voxel::BlockCollisionContactTolerance,
                1, "none"),
            makeContext(
                WorkloadKind::HighSpeedSweep,
                "high_speed_sweep",
                "single_full_cube_at_cell_200",
                m_highSpeedWorld,
                0.5f, 256.0f,
                200.0f - kEntityHalfExtent -
                    Voxel::BlockCollisionContactTolerance,
                1, "none"),
            makeContext(
                WorkloadKind::MultipleEntities,
                "multiple_independent_entities",
                "64_entities_against_independent_full_cube_cells",
                m_multipleWorld,
                0.5f, 4.0f,
                3.0f - kEntityHalfExtent -
                    Voxel::BlockCollisionContactTolerance,
                kMultipleEntityCount,
                "world_entities_iteration_snapshot"),
        }};
    }

    Voxel::WorldResources m_resources;
    Voxel::World m_emptyWorld;
    Voxel::World m_fullWorld;
    Voxel::World m_mixedWorld;
    Voxel::World m_highSpeedWorld;
    Voxel::World m_multipleWorld;
    Voxel::BlockID m_fullCubeId{};
    Voxel::BlockID m_singlePartialId{};
    Voxel::BlockID m_multiplePartialId{};
    GravityFreeEntity m_emptyEntity;
    GravityFreeEntity m_fullEntity;
    GravityFreeEntity m_mixedEntity;
    GravityFreeEntity m_highSpeedEntity;
    std::vector<GravityFreeEntity*> m_multipleEntities;
};

std::optional<size_t> parseBounded(
    std::string_view value,
    size_t minimum,
    size_t maximum
) {
    size_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} ||
        end != value.data() + value.size() ||
        parsed < minimum || parsed > maximum) {
        return std::nullopt;
    }
    return parsed;
}

void printUsage() {
    std::cout
        << "Usage: Rigel_entity_collision_benchmark [options]\n"
        << "  --iterations N   timed iterations per workload (default "
        << kDefaultIterations << ", maximum " << kMaximumIterations << ")\n"
        << "  --warmup N       warmup iterations per workload (default "
        << kDefaultWarmupIterations << ", maximum "
        << kMaximumWarmupIterations << ")\n"
        << "  --validate-only  validate synthetic outputs without timing\n";
}

ParseResult parseOptions(int argc, char** argv, Options& options) {
    bool iterationsSpecified = false;
    bool warmupSpecified = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            printUsage();
            return ParseResult::Help;
        }
        if (argument == "--validate-only") {
            options.validateOnly = true;
            continue;
        }
        if (argument != "--iterations" && argument != "--warmup") {
            std::cerr << "Unknown option: " << argument << '\n';
            return ParseResult::Error;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return ParseResult::Error;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--iterations") {
            if (iterationsSpecified) {
                std::cerr << "Iteration count may be specified only once\n";
                return ParseResult::Error;
            }
            const auto parsed = parseBounded(value, 1, kMaximumIterations);
            if (!parsed) {
                std::cerr << "Invalid bounded iteration count: " << value
                          << " (expected 1-" << kMaximumIterations << ")\n";
                return ParseResult::Error;
            }
            options.iterations = *parsed;
            iterationsSpecified = true;
        } else {
            if (warmupSpecified) {
                std::cerr << "Warmup count may be specified only once\n";
                return ParseResult::Error;
            }
            const auto parsed = parseBounded(
                value, 0, kMaximumWarmupIterations);
            if (!parsed) {
                std::cerr << "Invalid bounded warmup count: " << value
                          << " (expected 0-" << kMaximumWarmupIterations
                          << ")\n";
                return ParseResult::Error;
            }
            options.warmupIterations = *parsed;
            warmupSpecified = true;
        }
    }
    return ParseResult::Run;
}

Measurement measure(
    CollisionFixture& fixture,
    const WorkloadContext& workload,
    size_t warmupIterations,
    size_t iterations
) {
    uint64_t checksum = 0;
    for (size_t iteration = 0; iteration < warmupIterations; ++iteration) {
        checksum += fixture.run(workload.kind);
    }

    allocationCount = 0;
    const auto start = BenchmarkClock::now();
    countAllocations = true;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        checksum += fixture.run(workload.kind);
    }
    countAllocations = false;
    const auto end = BenchmarkClock::now();

    const uint64_t allocations = allocationCount;
    if (workload.allocationOwner == "none" && allocations != 0) {
        throw std::runtime_error(
            "collision query or resolver allocated during measurement");
    }
    if (workload.kind == WorkloadKind::MultipleEntities &&
        allocations != iterations) {
        throw std::runtime_error(
            "multiple-entity allocation accounting changed");
    }

    return {
        .elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(end - start).count(),
        .allocations = allocations,
        .checksum = checksum,
    };
}

int runBenchmark(int argc, char** argv) {
    Options options;
    const ParseResult parseResult = parseOptions(argc, argv, options);
    if (parseResult != ParseResult::Run) {
        return parseResult == ParseResult::Help ? 0 : 2;
    }

    CollisionFixture fixture;
    const auto workloads = fixture.validate();

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "benchmark name=entity_collision version=1"
        << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
        << " compiler=" << RIGEL_BENCHMARK_COMPILER
        << " system=" << RIGEL_BENCHMARK_SYSTEM
        << " processor=" << RIGEL_BENCHMARK_PROCESSOR
        << " hardware_threads=" << std::thread::hardware_concurrency()
        << " clock=steady_clock"
        << " clock_is_steady=" << (BenchmarkClock::is_steady ? "true" : "false")
        << " clock_period_num=" << BenchmarkClock::period::num
        << " clock_period_den=" << BenchmarkClock::period::den
        << " fixture=deterministic_synthetic_blocks"
        << " production_configuration=false\n";
    std::cout
        << "validation status=passed"
        << " fixture_count=" << workloads.size()
        << " canonical_full_cube=true"
        << " immutable_partial_box_spans=true"
        << " output_checked_before_timing=true"
        << " allocation_counter=global_operator_new_calls\n";

    if (options.validateOnly) {
        std::cout << "configuration mode=validate_only\n";
        return 0;
    }

    std::cout
        << "configuration mode=timed"
        << " iterations_per_workload=" << options.iterations
        << " warmup_iterations_per_workload=" << options.warmupIterations
        << " maximum_iterations=" << kMaximumIterations
        << " maximum_warmup_iterations=" << kMaximumWarmupIterations
        << " movement_axes_per_entity=1"
        << " collision_queries_per_entity=2"
        << " axis_order=x,y,z"
        << " allocation_timing_scope=workload_body\n";

    std::array<Measurement, 5> measurements{};
    uint64_t resultGuard = 0;
    for (size_t index = 0; index < workloads.size(); ++index) {
        measurements[index] = measure(
            fixture,
            workloads[index],
            options.warmupIterations,
            options.iterations);
        resultGuard ^= measurements[index].checksum;
    }

    const double emptyMillisecondsPerIteration =
        measurements.front().elapsedMilliseconds /
        static_cast<double>(options.iterations);
    for (size_t index = 0; index < workloads.size(); ++index) {
        const WorkloadContext& workload = workloads[index];
        const Measurement& measurement = measurements[index];
        const uint64_t entityUpdates =
            static_cast<uint64_t>(options.iterations) *
            workload.entitiesPerIteration;
        const double millisecondsPerIteration =
            measurement.elapsedMilliseconds /
            static_cast<double>(options.iterations);
        const double nanosecondsPerEntity =
            measurement.elapsedMilliseconds * 1'000'000.0 /
            static_cast<double>(entityUpdates);
        std::cout
            << "workload name=" << workload.name
            << " shape_context=" << workload.shapeContext
            << " iterations=" << options.iterations
            << " entities_per_iteration=" << workload.entitiesPerIteration
            << " entity_updates=" << entityUpdates
            << " broadphase_candidates_per_iteration="
            << workload.broadphaseCandidatesPerIteration
            << " validated_boxes_per_sweep="
            << workload.validatedBoxesPerSweep
            << " elapsed_ms=" << measurement.elapsedMilliseconds
            << " ns_per_entity_update=" << nanosecondsPerEntity
            << " relative_total_cost_to_empty="
            << (millisecondsPerIteration / emptyMillisecondsPerIteration)
            << " relative_per_entity_cost_to_empty="
            << (nanosecondsPerEntity /
                (emptyMillisecondsPerIteration * 1'000'000.0))
            << " allocations=" << measurement.allocations
            << " allocations_per_iteration="
            << (static_cast<double>(measurement.allocations) /
                static_cast<double>(options.iterations))
            << " allocation_owner=" << workload.allocationOwner
            << " result_checksum=" << measurement.checksum
            << '\n';
    }

    static volatile uint64_t benchmarkResultGuard = 0;
    benchmarkResultGuard = resultGuard;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return runBenchmark(argc, argv);
    } catch (const std::exception& error) {
        countAllocations = false;
        std::cerr << "entity_collision_benchmark_failed reason=\""
                  << error.what() << "\"\n";
    } catch (...) {
        countAllocations = false;
        std::cerr
            << "entity_collision_benchmark_failed reason=unknown_exception\n";
    }
    return 1;
}
