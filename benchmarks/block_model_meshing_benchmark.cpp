#include "Rigel/Voxel/MeshBuilder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
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

using namespace Rigel::Voxel;

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr size_t kDefaultIterations = 20;
constexpr size_t kMaximumIterations = 100;
constexpr size_t kDefaultWarmupIterations = 3;
constexpr size_t kMaximumWarmupIterations = 20;
constexpr int kFilledHeight = Chunk::SIZE / 2;

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

struct FixtureExpectation {
    size_t cubeBlocks = 0;
    size_t singleCuboidBlocks = 0;
    size_t multipleCuboidBlocks = 0;
    size_t vertices = 0;
    size_t indices = 0;
    std::array<size_t, RenderLayerCount> layerIndices{};
};

struct Fixture {
    std::string_view name;
    std::unique_ptr<Chunk> chunk;
    std::unique_ptr<std::array<BlockState, MeshBuilder::PaddedVolume>> padded;
    FixtureExpectation expected;
};

struct RegistryFixture {
    BlockRegistry registry;
    TextureAtlas atlas;
    BlockID cube;
    std::array<BlockID, 7> singleCuboids{};
    std::array<BlockID, 7> multipleCuboids{};
};

struct Measurement {
    std::vector<double> milliseconds;
    uint64_t resultChecksum = 0;
};

BlockModelFace inventedFace(BlockModelUvRotation rotation) {
    return BlockModelFace{
        .textureSlot = "invented_surface",
        .uv = {0.125f, 0.25f, 0.875f, 0.75f},
        .rotation = rotation,
        .ambientOcclusion = false,
        .cullAgainstOpaqueNeighbor = false,
    };
}

BlockModelCuboid inventedCuboid(
    BlockModelBounds bounds,
    BlockModelUvRotation rotation = BlockModelUvRotation::None
) {
    BlockModelCuboid cuboid;
    cuboid.bounds = bounds;
    for (auto& face : cuboid.faces) {
        face = inventedFace(rotation);
    }
    return cuboid;
}

std::shared_ptr<const BlockModel> makeSingleCuboidModel() {
    return std::make_shared<const BlockModel>(
        "benchmark:invented_post",
        std::vector<std::string>{"invented_surface"},
        std::vector<BlockModelCuboid>{inventedCuboid(
            {{0.25f, 0.0f, 0.25f}, {0.75f, 1.0f, 0.75f}})});
}

std::shared_ptr<const BlockModel> makeMultipleCuboidModel() {
    return std::make_shared<const BlockModel>(
        "benchmark:invented_stand",
        std::vector<std::string>{"invented_surface"},
        std::vector<BlockModelCuboid>{
            inventedCuboid(
                {{0.125f, 0.0f, 0.125f}, {0.875f, 0.25f, 0.875f}},
                BlockModelUvRotation::Quarter),
            inventedCuboid(
                {{0.375f, 0.25f, 0.375f}, {0.625f, 1.0f, 0.625f}},
                BlockModelUvRotation::ThreeQuarter),
        });
}

BlockType makeType(
    std::shared_ptr<const BlockModel> model,
    BlockModelOrientation orientation,
    RenderLayer layer
) {
    BlockType type;
    type.model = std::move(model);
    type.model.orientation = orientation;
    type.isOpaque = false;
    type.isSolid = true;
    type.layer = layer;
    type.lightAttenuation = 0;
    type.textures.bind(
        "invented_surface", "textures/benchmark/invented_surface.png");
    return type;
}

RegistryFixture makeRegistry() {
    RegistryFixture fixture;
    std::array<unsigned char, 16 * 16 * 4> pixels{};
    pixels.fill(255);
    fixture.atlas.addTexture(
        "textures/benchmark/invented_cube.png", pixels.data());
    fixture.atlas.addTexture(
        "textures/benchmark/invented_surface.png", pixels.data());

    BlockType cube;
    cube.model = BlockModel::fullCube();
    cube.isOpaque = true;
    cube.isSolid = true;
    cube.layer = RenderLayer::Opaque;
    cube.textures = FaceTextures::uniform(
        "textures/benchmark/invented_cube.png");
    fixture.cube = fixture.registry.registerBlock(
        "benchmark:canonical_cube", std::move(cube));

    const auto single = makeSingleCuboidModel();
    const auto multiple = makeMultipleCuboidModel();
    constexpr std::array<BlockModelOrientation, 7> orientations = {
        BlockModelOrientation::Identity,
        BlockModelOrientation::RotateX90,
        BlockModelOrientation::RotateX270,
        BlockModelOrientation::RotateY90,
        BlockModelOrientation::RotateY180,
        BlockModelOrientation::RotateY270,
        BlockModelOrientation::RotateZ90,
    };
    for (size_t index = 0; index < orientations.size(); ++index) {
        fixture.singleCuboids[index] = fixture.registry.registerBlock(
            "benchmark:single_" + std::to_string(index),
            makeType(single, orientations[index], RenderLayer::Cutout));
        fixture.multipleCuboids[index] = fixture.registry.registerBlock(
            "benchmark:multiple_" + std::to_string(index),
            makeType(multiple, orientations[index], RenderLayer::Transparent));
    }
    fixture.registry.freeze();
    return fixture;
}

std::unique_ptr<std::array<BlockState, MeshBuilder::PaddedVolume>>
makePaddedBlocks(const Chunk& chunk) {
    auto padded = std::make_unique<
        std::array<BlockState, MeshBuilder::PaddedVolume>>();
    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                const size_t index = static_cast<size_t>(x + 1)
                    + static_cast<size_t>(y + 1) * MeshBuilder::PaddedSize
                    + static_cast<size_t>(z + 1) * MeshBuilder::PaddedSize
                        * MeshBuilder::PaddedSize;
                (*padded)[index] = chunk.getBlock(x, y, z);
            }
        }
    }
    return padded;
}

Fixture makeFixture(
    std::string_view name,
    const RegistryFixture& registry,
    FixtureExpectation expectation
) {
    auto chunk = std::make_unique<Chunk>();
    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int y = 0; y < kFilledHeight; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                BlockID id = registry.cube;
                const size_t orientation = static_cast<size_t>(
                    (x + y + z) % registry.singleCuboids.size());
                if (name == "representative_mixed") {
                    if (x % 8 == 0) {
                        id = registry.singleCuboids[orientation];
                    } else if (x % 8 == 4) {
                        id = registry.multipleCuboids[orientation];
                    }
                } else if (name == "non_cube_heavy") {
                    id = x % 5 < 3
                        ? registry.singleCuboids[orientation]
                        : registry.multipleCuboids[orientation];
                }
                chunk->setBlock(x, y, z, BlockState{id});
            }
        }
    }

    auto padded = makePaddedBlocks(*chunk);
    return Fixture{name, std::move(chunk), std::move(padded), expectation};
}

std::array<Fixture, 3> makeFixtures(const RegistryFixture& registry) {
    FixtureExpectation allCube;
    allCube.cubeBlocks = 16'384;
    allCube.vertices = 16'384;
    allCube.indices = 24'576;
    allCube.layerIndices[static_cast<size_t>(RenderLayer::Opaque)] = 24'576;

    FixtureExpectation mixed;
    mixed.cubeBlocks = 12'288;
    mixed.singleCuboidBlocks = 2'048;
    mixed.multipleCuboidBlocks = 2'048;
    mixed.vertices = 189'440;
    mixed.indices = 284'160;
    mixed.layerIndices[static_cast<size_t>(RenderLayer::Opaque)] = 62'976;
    mixed.layerIndices[static_cast<size_t>(RenderLayer::Cutout)] = 73'728;
    mixed.layerIndices[static_cast<size_t>(RenderLayer::Transparent)] = 147'456;

    FixtureExpectation modelHeavy;
    modelHeavy.singleCuboidBlocks = 10'240;
    modelHeavy.multipleCuboidBlocks = 6'144;
    modelHeavy.vertices = 540'672;
    modelHeavy.indices = 811'008;
    modelHeavy.layerIndices[static_cast<size_t>(RenderLayer::Cutout)] = 368'640;
    modelHeavy.layerIndices[static_cast<size_t>(RenderLayer::Transparent)] =
        442'368;

    return {
        makeFixture("all_cube", registry, allCube),
        makeFixture("representative_mixed", registry, mixed),
        makeFixture("non_cube_heavy", registry, modelHeavy),
    };
}

MeshBuilder::BuildContext buildContext(
    const Fixture& fixture,
    const RegistryFixture& registry
) {
    return {
        .chunk = *fixture.chunk,
        .registry = registry.registry,
        .atlas = &registry.atlas,
        .paddedBlocks = fixture.padded.get(),
    };
}

void validateMesh(const Fixture& fixture, const ChunkMesh& mesh) {
    const size_t expectedBlocks = fixture.expected.cubeBlocks
        + fixture.expected.singleCuboidBlocks
        + fixture.expected.multipleCuboidBlocks;
    if (expectedBlocks !=
            static_cast<size_t>(Chunk::SIZE * kFilledHeight * Chunk::SIZE) ||
        fixture.chunk->nonAirCount() != expectedBlocks) {
        throw std::runtime_error(
            std::string(fixture.name) + " fixture block count changed");
    }
    if (mesh.vertexCount() != fixture.expected.vertices ||
        mesh.indexCount() != fixture.expected.indices) {
        throw std::runtime_error(
            std::string(fixture.name) + " mesh count mismatch: expected "
            + std::to_string(fixture.expected.vertices) + "/"
            + std::to_string(fixture.expected.indices) + ", got "
            + std::to_string(mesh.vertexCount()) + "/"
            + std::to_string(mesh.indexCount()));
    }
    if (mesh.vertexCount() % 4 != 0 || mesh.indexCount() % 6 != 0) {
        throw std::runtime_error(
            std::string(fixture.name) + " mesh is not composed of quads");
    }

    size_t expectedIndexStart = 0;
    for (size_t layer = 0; layer < mesh.layers.size(); ++layer) {
        const auto& range = mesh.layers[layer];
        if (range.indexStart != expectedIndexStart ||
            range.indexCount != fixture.expected.layerIndices[layer]) {
            throw std::runtime_error(
                std::string(fixture.name) + " render-layer range mismatch");
        }
        expectedIndexStart += range.indexCount;
    }
    if (expectedIndexStart != mesh.indexCount() ||
        std::any_of(
            mesh.indices.begin(), mesh.indices.end(),
            [&mesh](uint32_t index) { return index >= mesh.vertexCount(); })) {
        throw std::runtime_error(
            std::string(fixture.name) + " mesh contains an invalid index");
    }
}

void validateFixtures(
    const RegistryFixture& registry,
    std::span<const Fixture> fixtures
) {
    if (registry.atlas.textureCount() != 2) {
        throw std::runtime_error("invented texture atlas is incomplete");
    }
    const BlockType& cube = registry.registry.getType(registry.cube);
    if (!registry.registry.frozen() ||
        cube.model.geometry.get() != BlockModel::fullCube().get() ||
        !cube.model->isFullCube() ||
        cube.model.orientation != BlockModelOrientation::Identity) {
        throw std::runtime_error("canonical cube fast path is not active");
    }
    for (const BlockID id : registry.singleCuboids) {
        if (registry.registry.getType(id).model->isFullCube()) {
            throw std::runtime_error("single cuboid uses the cube fast path");
        }
    }
    for (const BlockID id : registry.multipleCuboids) {
        if (registry.registry.getType(id).model->isFullCube()) {
            throw std::runtime_error("multiple cuboid uses the cube fast path");
        }
    }

    const MeshBuilder builder;
    for (const Fixture& fixture : fixtures) {
        validateMesh(
            fixture, builder.build(buildContext(fixture, registry)));
    }
}

uint64_t meshChecksum(const ChunkMesh& mesh) {
    uint64_t checksum = mesh.vertexCount() * UINT64_C(0x9e3779b185ebca87);
    checksum ^= mesh.indexCount() * UINT64_C(0xc2b2ae3d27d4eb4f);
    for (const auto& layer : mesh.layers) {
        checksum ^= static_cast<uint64_t>(layer.indexStart) << 32;
        checksum ^= layer.indexCount;
    }
    return checksum;
}

Measurement measure(
    const Fixture& fixture,
    const RegistryFixture& registry,
    size_t warmupIterations,
    size_t iterations
) {
    const MeshBuilder builder;
    const auto context = buildContext(fixture, registry);
    for (size_t iteration = 0; iteration < warmupIterations; ++iteration) {
        const ChunkMesh mesh = builder.build(context);
        if (mesh.vertexCount() != fixture.expected.vertices ||
            mesh.indexCount() != fixture.expected.indices) {
            throw std::runtime_error(
                std::string(fixture.name) + " warmup output changed");
        }
    }

    Measurement measurement;
    measurement.milliseconds.reserve(iterations);
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto start = BenchmarkClock::now();
        ChunkMesh mesh = builder.build(context);
        const auto end = BenchmarkClock::now();
        measurement.milliseconds.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
        if (mesh.vertexCount() != fixture.expected.vertices ||
            mesh.indexCount() != fixture.expected.indices) {
            throw std::runtime_error(
                std::string(fixture.name) + " timed output changed");
        }
        measurement.resultChecksum ^= meshChecksum(mesh)
            + iteration * UINT64_C(0x9e3779b97f4a7c15);
    }
    return measurement;
}

double nearestRank(std::vector<double> values, double percentile) {
    std::sort(values.begin(), values.end());
    const size_t rank = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(percentile * values.size())));
    return values[rank - 1];
}

void printMeasurement(
    const Fixture& fixture,
    const Measurement& measurement
) {
    const auto [minimum, maximum] = std::minmax_element(
        measurement.milliseconds.begin(), measurement.milliseconds.end());
    const double total = std::accumulate(
        measurement.milliseconds.begin(), measurement.milliseconds.end(), 0.0);
    std::cout
        << "workload name=" << fixture.name
        << " non_air_blocks=" << fixture.chunk->nonAirCount()
        << " canonical_cube_blocks=" << fixture.expected.cubeBlocks
        << " single_cuboid_blocks=" << fixture.expected.singleCuboidBlocks
        << " multiple_cuboid_blocks="
        << fixture.expected.multipleCuboidBlocks
        << " vertices=" << fixture.expected.vertices
        << " indices=" << fixture.expected.indices
        << " iterations=" << measurement.milliseconds.size()
        << " total_ms=" << total
        << " mean_ms=" << total / measurement.milliseconds.size()
        << " min_ms=" << *minimum
        << " p50_ms=" << nearestRank(measurement.milliseconds, 0.50)
        << " p95_ms=" << nearestRank(measurement.milliseconds, 0.95)
        << " max_ms=" << *maximum
        << " result_checksum=" << measurement.resultChecksum
        << '\n';
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
        << "Usage: Rigel_block_model_meshing_benchmark [options]\n"
        << "  --iterations N   timed builds per workload (1-"
        << kMaximumIterations << ", default " << kDefaultIterations << ")\n"
        << "  --warmup N       untimed builds per workload (0-"
        << kMaximumWarmupIterations << ", default "
        << kDefaultWarmupIterations << ")\n"
        << "  --validate-only  validate fixture meshes without timing\n";
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
            if (options.validateOnly) {
                std::cerr << "Validation mode may be specified only once\n";
                return ParseResult::Error;
            }
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
            const auto parsed = parseBounded(
                value, 1, kMaximumIterations);
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
                          << " (expected 0-" << kMaximumWarmupIterations << ")\n";
                return ParseResult::Error;
            }
            options.warmupIterations = *parsed;
            warmupSpecified = true;
        }
    }
    return ParseResult::Run;
}

int runBenchmark(int argc, char** argv) {
    Options options;
    const ParseResult parseResult = parseOptions(argc, argv, options);
    if (parseResult != ParseResult::Run) {
        return parseResult == ParseResult::Help ? 0 : 2;
    }

    RegistryFixture registry = makeRegistry();
    const auto fixtures = makeFixtures(registry);
    validateFixtures(registry, fixtures);

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "benchmark name=block_model_meshing version=1"
        << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
        << " compiler=" << RIGEL_BENCHMARK_COMPILER
        << " system=" << RIGEL_BENCHMARK_SYSTEM
        << " processor=" << RIGEL_BENCHMARK_PROCESSOR
        << " hardware_threads=" << std::thread::hardware_concurrency()
        << " clock=steady_clock"
        << " clock_is_steady=" << (BenchmarkClock::is_steady ? "true" : "false")
        << " clock_period_num=" << BenchmarkClock::period::num
        << " clock_period_den=" << BenchmarkClock::period::den
        << " chunk_size=" << Chunk::SIZE
        << " filled_height=" << kFilledHeight
        << " texture_atlas_entries=" << registry.atlas.textureCount()
        << " texture_atlas_upload=cpu_only"
        << " fixture=deterministic_invented_cuboids"
        << " production_configuration=false\n";
    std::cout
        << "validation status=passed"
        << " canonical_cube_fast_path=true"
        << " registry_frozen=true"
        << " fixture_count=" << fixtures.size()
        << " output_checked_before_timing=true\n";

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
        << " percentile_method=nearest_rank"
        << " workload_order=all_cube,representative_mixed,non_cube_heavy\n";

    uint64_t resultGuard = 0;
    for (const Fixture& fixture : fixtures) {
        const Measurement measurement = measure(
            fixture, registry, options.warmupIterations,
            options.iterations);
        resultGuard ^= measurement.resultChecksum;
        printMeasurement(fixture, measurement);
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
        std::cerr << "block_model_meshing_benchmark_failed reason=\""
                  << error.what() << "\"\n";
    } catch (...) {
        std::cerr
            << "block_model_meshing_benchmark_failed reason=unknown_exception\n";
    }
    return 1;
}
