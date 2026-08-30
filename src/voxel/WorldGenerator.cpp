#include "Rigel/Voxel/WorldGenerator.h"

#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/DensityFunction.h"
#include "Rigel/Voxel/Noise.h"
#include "Rigel/Voxel/WorldGenStages.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {
constexpr int kClimateColumnCount = Chunk::SIZE * Chunk::SIZE;
constexpr int kDefaultNoiseSampleStep = 4;
static_assert(Chunk::SIZE % kDefaultNoiseSampleStep == 0,
              "Chunk size must be divisible by default noise sample step.");

struct LocalWorldYRange {
    int first = 0;
    int onePastLast = 0;

    bool empty() const {
        return first >= onePastLast;
    }
};

LocalWorldYRange localWorldYRange(
    ChunkCoord coord,
    const GeneratorDefinitionData::Bounds& world) {
    const int64_t chunkMinY =
        static_cast<int64_t>(coord.y) * Chunk::SIZE;
    const int64_t first = std::clamp<int64_t>(
        static_cast<int64_t>(world.minY) - chunkMinY,
        0,
        Chunk::SIZE);
    const int64_t onePastLast = std::clamp<int64_t>(
        static_cast<int64_t>(world.maxY) - chunkMinY + 1,
        0,
        Chunk::SIZE);
    return {
        static_cast<int>(first),
        static_cast<int>(onePastLast)
    };
}

struct ChunkWorldAxisRange {
    int64_t first = 0;
    int64_t last = 0;
};

ChunkWorldAxisRange chunkWorldAxisRange(int chunkCoordinate) {
    const int64_t first =
        static_cast<int64_t>(chunkCoordinate) * Chunk::SIZE;
    return {first, first + Chunk::SIZE - 1};
}

void requireSupportedChunkCoordinates(ChunkCoord coord,
                                      bool structuresEnabled) {
    const auto requireAxis = [](ChunkWorldAxisRange range,
                                std::string_view axis) {
        if (range.first < std::numeric_limits<int>::min() ||
            range.last > std::numeric_limits<int>::max()) {
            throw std::out_of_range(
                "Chunk " + std::string(axis) +
                " coordinate is outside the supported world-coordinate "
                "range");
        }
    };
    const ChunkWorldAxisRange x = chunkWorldAxisRange(coord.x);
    const ChunkWorldAxisRange y = chunkWorldAxisRange(coord.y);
    const ChunkWorldAxisRange z = chunkWorldAxisRange(coord.z);
    requireAxis(x, "X");
    requireAxis(y, "Y");
    requireAxis(z, "Z");
    if (structuresEnabled &&
        (x.last + 11 > std::numeric_limits<int>::max() ||
         z.first - 7 < std::numeric_limits<int>::min())) {
        throw std::out_of_range(
            "Chunk coordinate is outside the supported structure-sampling "
            "range");
    }
}

void clearOutsideWorldYRange(
    ChunkBuffer& buffer,
    LocalWorldYRange range) {
    for (int z = 0; z < Chunk::SIZE; ++z) {
        for (int y = 0; y < range.first; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                buffer.at(x, y, z) = BlockState{};
            }
        }
        for (int y = range.onePastLast; y < Chunk::SIZE; ++y) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                buffer.at(x, y, z) = BlockState{};
            }
        }
    }
}

int columnIndex(int x, int z) {
    return x + z * Chunk::SIZE;
}

struct BiomeSelectionScore {
    long double logWeight = 0.0L;
    long double logDistance = 0.0L;
};

long double relativeBiomeLogWeight(
    const BiomeSelectionScore& candidate,
    const BiomeSelectionScore& reference,
    float blendPower) {
    // Compare the ratio directly so a very large shared distance exponent
    // cannot erase the authored weight difference through cancellation.
    return candidate.logWeight - reference.logWeight -
        static_cast<long double>(blendPower) *
            (candidate.logDistance - reference.logDistance);
}

int normalizeSampleStep(int step) {
    if (step <= 0 || (Chunk::SIZE % step) != 0) {
        return kDefaultNoiseSampleStep;
    }
    return step;
}

struct NoiseGrid {
    bool valid = false;
    int originX = 0;
    int originY = 0;
    int originZ = 0;
    int step = kDefaultNoiseSampleStep;
    int count = 0;
    std::vector<float> values;

    void build(int originXIn, int originYIn, int originZIn, uint32_t seed, int sampleStep,
               const GeneratorDefinitionData::Noise& config) {
        originX = originXIn;
        originY = originYIn;
        originZ = originZIn;
        step = normalizeSampleStep(sampleStep);
        count = Chunk::SIZE / step + 1;
        values.resize(count * count * count);
        for (int z = 0; z < count; ++z) {
            int worldZ = originZ + z * step;
            for (int y = 0; y < count; ++y) {
                int worldY = originY + y * step;
                for (int x = 0; x < count; ++x) {
                    int worldX = originX + x * step;
                    values[index(x, y, z)] = Noise::fbm3D(
                        static_cast<float>(worldX),
                        static_cast<float>(worldY),
                        static_cast<float>(worldZ),
                        seed,
                        config
                    );
                }
            }
        }
        valid = true;
    }

    float sample(int worldX, int worldY, int worldZ) const {
        if (!valid) {
            return 0.0f;
        }
        int lx = worldX - originX;
        int ly = worldY - originY;
        int lz = worldZ - originZ;
        int ix = std::clamp(lx / step, 0, count - 2);
        int iy = std::clamp(ly / step, 0, count - 2);
        int iz = std::clamp(lz / step, 0, count - 2);
        float tx = static_cast<float>(lx - ix * step)
            / static_cast<float>(step);
        float ty = static_cast<float>(ly - iy * step)
            / static_cast<float>(step);
        float tz = static_cast<float>(lz - iz * step)
            / static_cast<float>(step);
        tx = std::clamp(tx, 0.0f, 1.0f);
        ty = std::clamp(ty, 0.0f, 1.0f);
        tz = std::clamp(tz, 0.0f, 1.0f);

        float c000 = values[index(ix, iy, iz)];
        float c100 = values[index(ix + 1, iy, iz)];
        float c010 = values[index(ix, iy + 1, iz)];
        float c110 = values[index(ix + 1, iy + 1, iz)];
        float c001 = values[index(ix, iy, iz + 1)];
        float c101 = values[index(ix + 1, iy, iz + 1)];
        float c011 = values[index(ix, iy + 1, iz + 1)];
        float c111 = values[index(ix + 1, iy + 1, iz + 1)];

        float x00 = lerp(c000, c100, tx);
        float x10 = lerp(c010, c110, tx);
        float x01 = lerp(c001, c101, tx);
        float x11 = lerp(c011, c111, tx);
        float y0 = lerp(x00, x10, ty);
        float y1 = lerp(x01, x11, ty);
        return lerp(y0, y1, tz);
    }

private:
    int index(int x, int y, int z) const {
        return x + y * count + z * count * count;
    }

    static float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }
};

struct NoiseGridCache final : DensitySampleContext::NoiseSampleCache {
    explicit NoiseGridCache(const DensityGraph* graph, uint32_t seed, ChunkCoord coord,
                            int sampleStep = kDefaultNoiseSampleStep) {
        if (!graph || graph->nodes.empty()) {
            return;
        }
        const int64_t originXWide =
            static_cast<int64_t>(coord.x) * Chunk::SIZE;
        const int64_t originYWide =
            static_cast<int64_t>(coord.y) * Chunk::SIZE;
        const int64_t originZWide =
            static_cast<int64_t>(coord.z) * Chunk::SIZE;
        if (originXWide < std::numeric_limits<int>::min() ||
            originYWide < std::numeric_limits<int>::min() ||
            originZWide < std::numeric_limits<int>::min() ||
            originXWide + Chunk::SIZE > std::numeric_limits<int>::max() ||
            originYWide + Chunk::SIZE > std::numeric_limits<int>::max() ||
            originZWide + Chunk::SIZE > std::numeric_limits<int>::max()) {
            return;
        }
        const int originX = static_cast<int>(originXWide);
        const int originY = static_cast<int>(originYWide);
        const int originZ = static_cast<int>(originZWide);
        int step = normalizeSampleStep(sampleStep);
        grids.resize(graph->nodes.size());
        for (size_t i = 0; i < graph->nodes.size(); ++i) {
            const auto& node = graph->nodes[i];
            if (node.type != DensityNodeType::Noise3D) {
                continue;
            }
            uint32_t nodeSeed = Noise::seedForChannel(seed, node.name);
            grids[i].build(originX, originY, originZ, nodeSeed, step, node.noise);
        }
    }

    bool sampleNoise3D(int nodeIndex, int worldX, int worldY, int worldZ,
                       float& outValue) const override {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(grids.size())) {
            return false;
        }
        const NoiseGrid& grid = grids[static_cast<size_t>(nodeIndex)];
        if (!grid.valid) {
            return false;
        }
        outValue = grid.sample(worldX, worldY, worldZ);
        return true;
    }

    std::vector<NoiseGrid> grids;
};

struct VerticalNoiseGridCache final : DensitySampleContext::NoiseSampleCache {
    VerticalNoiseGridCache(const DensityGraph* graph,
                           uint32_t seed,
                           ChunkCoord horizontalCoord,
                           int sampleStep = kDefaultNoiseSampleStep)
        : graph(graph)
        , seed(seed)
        , horizontalCoord(horizontalCoord)
        , sampleStep(sampleStep)
    {}

    bool sampleNoise3D(int nodeIndex,
                       int worldX,
                       int worldY,
                       int worldZ,
                       float& outValue) const override {
        const int chunkY = worldToChunk(worldX, worldY, worldZ).y;
        auto [it, inserted] = grids.try_emplace(
            chunkY,
            graph,
            seed,
            ChunkCoord{horizontalCoord.x, chunkY, horizontalCoord.z},
            sampleStep);
        static_cast<void>(inserted);
        return it->second.sampleNoise3D(
            nodeIndex, worldX, worldY, worldZ, outValue);
    }

    const DensityGraph* graph = nullptr;
    uint32_t seed = 0;
    ChunkCoord horizontalCoord;
    int sampleStep = kDefaultNoiseSampleStep;
    mutable std::unordered_map<int, NoiseGridCache> grids;
};

class ClimateGlobalStage : public WorldGenStage {
public:
    ClimateGlobalStage(const GeneratorDefinitionData& config, uint32_t seed)
        : m_definition(config)
        , m_temperatureSeed(Noise::seedForChannel(seed, "climate_global/temperature"))
        , m_humiditySeed(Noise::seedForChannel(seed, "climate_global/humidity"))
        , m_continentalnessSeed(Noise::seedForChannel(seed, "climate_global/continentalness"))
    {}

    const char* name() const override { return "climate_global"; }

    void apply(WorldGenContext& ctx, ChunkBuffer&) const override {
        const auto& climate = m_definition.climate;
        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int worldX = ctx.coord.x * Chunk::SIZE + x;
                int worldZ = ctx.coord.z * Chunk::SIZE + z;
                ClimateSample sample;
                sample.temperature = Noise::fbm2D(
                    static_cast<float>(worldX),
                    static_cast<float>(worldZ),
                    m_temperatureSeed,
                    climate.global.temperature
                );
                sample.humidity = Noise::fbm2D(
                    static_cast<float>(worldX),
                    static_cast<float>(worldZ),
                    m_humiditySeed,
                    climate.global.humidity
                );
                sample.continentalness = Noise::fbm2D(
                    static_cast<float>(worldX),
                    static_cast<float>(worldZ),
                    m_continentalnessSeed,
                    climate.global.continentalness
                );

                if (climate.latitudeScale != 0.0f && climate.latitudeStrength != 0.0f) {
                    float latitude = std::clamp(
                        static_cast<float>(worldZ) * climate.latitudeScale,
                        -1.0f,
                        1.0f
                    );
                    sample.temperature += latitude * climate.latitudeStrength;
                }

                ctx.climate[columnIndex(x, z)] = sample;
            }
        }
    }

private:
    const GeneratorDefinitionData& m_definition;
    uint32_t m_temperatureSeed = 0;
    uint32_t m_humiditySeed = 0;
    uint32_t m_continentalnessSeed = 0;
};

class ClimateLocalStage : public WorldGenStage {
public:
    ClimateLocalStage(const GeneratorDefinitionData& config, uint32_t seed)
        : m_definition(config)
        , m_temperatureSeed(Noise::seedForChannel(seed, "climate_local/temperature"))
        , m_humiditySeed(Noise::seedForChannel(seed, "climate_local/humidity"))
        , m_continentalnessSeed(Noise::seedForChannel(seed, "climate_local/continentalness"))
    {}

    const char* name() const override { return "climate_local"; }

    void apply(WorldGenContext& ctx, ChunkBuffer&) const override {
        const auto& climate = m_definition.climate;
        if (climate.localBlend == 0.0f) {
            return;
        }
        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int worldX = ctx.coord.x * Chunk::SIZE + x;
                int worldZ = ctx.coord.z * Chunk::SIZE + z;
                ClimateSample local;
                local.temperature = Noise::fbm2D(
                    static_cast<float>(worldX),
                    static_cast<float>(worldZ),
                    m_temperatureSeed,
                    climate.local.temperature
                );
                local.humidity = Noise::fbm2D(
                    static_cast<float>(worldX),
                    static_cast<float>(worldZ),
                    m_humiditySeed,
                    climate.local.humidity
                );
                local.continentalness = Noise::fbm2D(
                    static_cast<float>(worldX),
                    static_cast<float>(worldZ),
                    m_continentalnessSeed,
                    climate.local.continentalness
                );

                ClimateSample& sample = ctx.climate[columnIndex(x, z)];
                sample.temperature += local.temperature * climate.localBlend;
                sample.humidity += local.humidity * climate.localBlend;
                sample.continentalness += local.continentalness * climate.localBlend;
            }
        }
    }

private:
    const GeneratorDefinitionData& m_definition;
    uint32_t m_temperatureSeed = 0;
    uint32_t m_humiditySeed = 0;
    uint32_t m_continentalnessSeed = 0;
};

class BiomeResolveStage : public WorldGenStage {
public:
    explicit BiomeResolveStage(const GeneratorDefinitionData& config)
        : m_definition(config)
    {
        const auto& coast = m_definition.biomes.coast;
        m_coastMin = coast.minContinentalness;
        m_coastMax = coast.maxContinentalness;
        for (size_t i = 0; i < m_definition.biomes.entries.size(); ++i) {
            if (m_definition.biomes.entries[i].id == coast.biome) {
                m_coastBiomeIndex = static_cast<int>(i);
                break;
            }
        }
        if (m_coastBiomeIndex < 0) {
            throw std::logic_error("Validated coast biome is unavailable");
        }
    }

    const char* name() const override { return "biome_resolve"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) const override {
        (void)buffer;
        const auto& biomes = m_definition.biomes;
        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int index = columnIndex(x, z);
                const ClimateSample& sample = ctx.climate[index];

                int bestIndex = -1;
                int secondIndex = -1;
                BiomeSelectionScore bestScore;
                BiomeSelectionScore secondScore;
                bool coastActive = sample.continentalness >= m_coastMin
                    && sample.continentalness <= m_coastMax;

                for (size_t i = 0; i < biomes.entries.size(); ++i) {
                    if (!coastActive &&
                        static_cast<int>(i) == m_coastBiomeIndex) {
                        continue;
                    }
                    const auto& biome = biomes.entries[i];
                    const long double dt =
                        static_cast<long double>(sample.temperature) -
                        static_cast<long double>(biome.target.temperature);
                    const long double dh =
                        static_cast<long double>(sample.humidity) -
                        static_cast<long double>(biome.target.humidity);
                    const long double dc =
                        static_cast<long double>(sample.continentalness) -
                        static_cast<long double>(biome.target.continentalness);
                    const long double distance = std::hypot(dt, dh, dc);
                    const BiomeSelectionScore score{
                        std::log(static_cast<long double>(biome.weight)),
                        std::log(
                            distance +
                            static_cast<long double>(biomes.epsilon))};
                    if (bestIndex < 0 ||
                        relativeBiomeLogWeight(
                            score, bestScore, biomes.blendPower) > 0.0L) {
                        secondScore = bestScore;
                        secondIndex = bestIndex;
                        bestScore = score;
                        bestIndex = static_cast<int>(i);
                    } else if (
                        secondIndex < 0 ||
                        relativeBiomeLogWeight(
                            score, secondScore, biomes.blendPower) > 0.0L) {
                        secondScore = score;
                        secondIndex = static_cast<int>(i);
                    }
                }

                if (coastActive) {
                    secondIndex = bestIndex;
                    bestIndex = m_coastBiomeIndex;
                }

                BiomeSample result;
                result.primary = bestIndex;
                result.secondary = secondIndex;
                if (!coastActive && secondIndex >= 0) {
                    const long double relativeSecond = std::exp(
                        relativeBiomeLogWeight(
                            secondScore, bestScore, biomes.blendPower));
                    result.blend = static_cast<float>(
                        relativeSecond / (1.0L + relativeSecond));
                }
                ctx.biomes[static_cast<size_t>(index)] = result;
            }
        }
    }

private:
    const GeneratorDefinitionData& m_definition;
    int m_coastBiomeIndex = -1;
    float m_coastMin = 0.0f;
    float m_coastMax = 0.0f;
};

class TerrainDensityStage : public WorldGenStage {
public:
    TerrainDensityStage(const GeneratorDefinitionData& config,
                        uint32_t seed,
                        const DensityGraph* graph)
        : m_definition(config)
        , m_seed(seed)
        , m_graph(graph)
    {}

    const char* name() const override { return "terrain_density"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) const override {
        const auto& terrain = m_definition.terrain;
        const auto& bounds = m_definition.bounds;

        buffer.blocks.fill(BlockState{});
        DensityEvaluator evaluator(m_graph, m_seed);
        NoiseGridCache noiseCache(m_graph, m_seed, ctx.coord);

        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int worldX = ctx.coord.x * Chunk::SIZE + x;
                int worldZ = ctx.coord.z * Chunk::SIZE + z;
                int index = columnIndex(x, z);
                int biomeIndex = ctx.biomes[static_cast<size_t>(index)].primary;
                if (biomeIndex < 0 ||
                    biomeIndex >= static_cast<int>(m_definition.biomes.entries.size())) {
                    throw std::logic_error("Validated biome selection is unavailable");
                }
                const bool allowWater = m_definition.biomes.entries[
                    static_cast<size_t>(biomeIndex)].waterFill;
                int maxSolid = bounds.minY - 1;

                for (int y = 0; y < Chunk::SIZE; ++y) {
                    int worldY = ctx.coord.y * Chunk::SIZE + y;
                    DensitySampleContext sampleCtx{
                        .worldX = worldX,
                        .worldY = worldY,
                        .worldZ = worldZ,
                        .climate = &ctx.climate[static_cast<size_t>(index)],
                        .noiseCache = &noiseCache
                    };
                    evaluator.beginSample();
                    const bool solid = evaluator.evaluateOutput(
                        terrain.densityOutput, sampleCtx) >= 0.0f;

                    if (solid) {
                        BlockState state;
                        state.id = ctx.solidBlock;
                        buffer.at(x, y, z) = state;
                        if (worldY > maxSolid) {
                            maxSolid = worldY;
                        }
                    } else if (allowWater && worldY <= terrain.seaLevel) {
                        BlockState state;
                        state.id = ctx.waterBlock;
                        buffer.at(x, y, z) = state;
                    }
                }

                ctx.heightMap[index] = maxSolid;
            }
        }
    }

private:
    const GeneratorDefinitionData& m_definition;
    uint32_t m_seed = 0;
    const DensityGraph* m_graph = nullptr;
};

class CavesStage : public WorldGenStage {
public:
    CavesStage(const GeneratorDefinitionData& config,
               uint32_t seed,
               const DensityGraph* graph)
        : m_definition(config)
        , m_seed(seed)
        , m_graph(graph)
    {}

    const char* name() const override { return "caves"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) const override {
        const auto& caves = m_definition.caves;
        DensityEvaluator evaluator(m_graph, m_seed);
        NoiseGridCache noiseCache(m_graph, m_seed, ctx.coord);

        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int worldX = ctx.coord.x * Chunk::SIZE + x;
                int worldZ = ctx.coord.z * Chunk::SIZE + z;
                int index = columnIndex(x, z);
                for (int y = 0; y < Chunk::SIZE; ++y) {
                    int worldY = ctx.coord.y * Chunk::SIZE + y;
                    BlockState& state = buffer.at(x, y, z);
                    if (state.isAir()) {
                        continue;
                    }
                    DensitySampleContext sampleCtx{
                        .worldX = worldX,
                        .worldY = worldY,
                        .worldZ = worldZ,
                        .climate = &ctx.climate[static_cast<size_t>(index)],
                        .noiseCache = &noiseCache
                    };
                    evaluator.beginSample();
                    float density = evaluator.evaluateOutput(caves.densityOutput, sampleCtx);
                    if (density > caves.threshold) {
                        state = BlockState{};
                    }
                }
            }
        }
    }

private:
    const GeneratorDefinitionData& m_definition;
    uint32_t m_seed = 0;
    const DensityGraph* m_graph = nullptr;
};

class SurfaceRulesStage : public WorldGenStage {
public:
    SurfaceRulesStage(const GeneratorDefinitionData& config,
                      uint32_t seed,
                      const BlockRegistry& registry,
                      const DensityGraph* graph)
        : m_definition(config)
        , m_seed(seed)
        , m_graph(graph) {
        m_surfaceByBiome.reserve(config.biomes.entries.size());
        for (const auto& biome : config.biomes.entries) {
            std::vector<ResolvedLayer> layers;
            for (const auto& layer : biome.surface) {
                auto blockId = registry.findByIdentifier(layer.material);
                if (!blockId) {
                    throw std::logic_error(
                        "Validated biome surface material is unavailable: " +
                        layer.material);
                }
                layers.push_back({*blockId, layer.depth});
            }
            m_surfaceByBiome.push_back(std::move(layers));
        }
    }

    const char* name() const override { return "surface_rules"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) const override {
        const auto& bounds = m_definition.bounds;
        DensityEvaluator evaluator(m_graph, m_seed);
        VerticalNoiseGridCache noiseCache(m_graph, m_seed, ctx.coord);

        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int index = columnIndex(x, z);
                int height = findSurfaceHeightGlobal(
                    ctx, x, z, evaluator, noiseCache
                );
                ctx.heightMap[index] = height;
                if (height < bounds.minY) {
                    continue;
                }

                const std::vector<ResolvedLayer>* layers = nullptr;
                int biomeIndex = ctx.biomes[index].primary;
                if (biomeIndex >= 0 && biomeIndex < static_cast<int>(m_surfaceByBiome.size())) {
                    layers = &m_surfaceByBiome[static_cast<size_t>(biomeIndex)];
                }

                if (!layers) {
                    throw std::logic_error(
                        "Validated biome surface selection is unavailable");
                }

                int depthOffset = 0;
                for (const auto& layer : *layers) {
                    if (layer.depth <= 0) {
                        continue;
                    }
                    for (int d = 0; d < layer.depth; ++d) {
                        int worldY = height - depthOffset;
                        if (worldY < bounds.minY) {
                            break;
                        }
                        int localY = worldY - ctx.coord.y * Chunk::SIZE;
                        if (localY >= 0 && localY < Chunk::SIZE) {
                            BlockState state;
                            state.id = layer.block;
                            buffer.at(x, localY, z) = state;
                        }
                        ++depthOffset;
                    }
                }
            }
        }
    }

private:
    struct ResolvedLayer {
        BlockID block;
        int depth = 1;
    };

    int findSurfaceHeightGlobal(const WorldGenContext& ctx, int x, int z,
                                DensityEvaluator& evaluator,
                                const DensitySampleContext::NoiseSampleCache&
                                    noiseCache) const {
        const auto& bounds = m_definition.bounds;
        int worldX = ctx.coord.x * Chunk::SIZE + x;
        int worldZ = ctx.coord.z * Chunk::SIZE + z;
        for (int worldY = bounds.maxY; worldY >= bounds.minY; --worldY) {
            if (isSolidAt(
                    ctx, worldX, worldY, worldZ, evaluator, noiseCache)) {
                return worldY;
            }
        }
        return bounds.minY - 1;
    }

    bool isSolidAt(const WorldGenContext& ctx,
                   int worldX,
                   int worldY,
                   int worldZ,
                   DensityEvaluator& evaluator,
                   const DensitySampleContext::NoiseSampleCache&
                       noiseCache) const {
        const auto& bounds = m_definition.bounds;
        if (worldY < bounds.minY) {
            return false;
        }
        int index = columnIndex(worldX - ctx.coord.x * Chunk::SIZE,
                                worldZ - ctx.coord.z * Chunk::SIZE);
        DensitySampleContext sampleCtx{
            .worldX = worldX,
            .worldY = worldY,
            .worldZ = worldZ,
            .climate = &ctx.climate[static_cast<size_t>(index)],
            .noiseCache = &noiseCache
        };
        evaluator.beginSample();
        if (evaluator.evaluateOutput(
                m_definition.terrain.densityOutput, sampleCtx) < 0.0f) {
            return false;
        }
        return !m_definition.caves.enabled ||
            evaluator.evaluateOutput(
                m_definition.caves.densityOutput, sampleCtx) <=
                m_definition.caves.threshold;
    }

    const GeneratorDefinitionData& m_definition;
    uint32_t m_seed = 0;
    const DensityGraph* m_graph = nullptr;
    std::vector<std::vector<ResolvedLayer>> m_surfaceByBiome;
};

class StructuresStage : public WorldGenStage {
public:
    StructuresStage(const GeneratorDefinitionData& config,
                    uint32_t seed,
                    const BlockRegistry& registry)
        : m_definition(config) {
        for (const auto& feature : config.structures.features) {
            auto blockId = registry.findByIdentifier(feature.material);
            if (!blockId) {
                throw std::logic_error(
                    "Validated structure material is unavailable: " +
                    feature.material);
            }
            FeatureResolved resolved;
            resolved.name = feature.id;
            resolved.block = *blockId;
            resolved.chance = feature.chance;
            resolved.minHeight = feature.minHeight;
            resolved.maxHeight = feature.maxHeight;
            resolved.seed = Noise::seedForChannel(seed, "feature/" + feature.id);
            if (!feature.biomes.empty()) {
                for (const auto& biomeName : feature.biomes) {
                    for (size_t i = 0; i < config.biomes.entries.size(); ++i) {
                        if (config.biomes.entries[i].id == biomeName) {
                            resolved.biomeIndices.push_back(static_cast<int>(i));
                            break;
                        }
                    }
                }
            }
            m_features.push_back(std::move(resolved));
        }
    }

    const char* name() const override { return "structures"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) const override {
        if (m_features.empty()) {
            return;
        }
        const auto& bounds = m_definition.bounds;

        for (int z = 0; z < Chunk::SIZE; ++z) {
            if (ctx.shouldCancel()) {
                return;
            }
            for (int x = 0; x < Chunk::SIZE; ++x) {
                if (ctx.shouldCancel()) {
                    return;
                }
                int index = columnIndex(x, z);
                int height = ctx.heightMap[index];
                if (height < bounds.minY) {
                    continue;
                }
                int biomeIndex = ctx.biomes[index].primary;

                int worldX = ctx.coord.x * Chunk::SIZE + x;
                int worldZ = ctx.coord.z * Chunk::SIZE + z;

                for (const auto& feature : m_features) {
                    if (feature.chance <= 0.0f) {
                        continue;
                    }
                    if (!feature.biomeIndices.empty()) {
                        bool allowed = false;
                        for (int biome : feature.biomeIndices) {
                            if (biome == biomeIndex) {
                                allowed = true;
                                break;
                            }
                        }
                        if (!allowed) {
                            continue;
                        }
                    }

                    float noise = Noise::noise2D(
                        static_cast<float>(worldX),
                        static_cast<float>(worldZ),
                        feature.seed
                    );
                    float chance = (noise + 1.0f) * 0.5f;
                    if (chance >= feature.chance) {
                        continue;
                    }

                    float heightNoise = Noise::noise2D(
                        static_cast<float>(worldX + 11),
                        static_cast<float>(worldZ - 7),
                        feature.seed ^ 0x9e3779b9u
                    );
                    const int64_t heightRange =
                        static_cast<int64_t>(feature.maxHeight) -
                        feature.minHeight;
                    int64_t pillarHeight = feature.minHeight;
                    if (heightRange > 0) {
                        const double t =
                            (static_cast<double>(heightNoise) + 1.0) * 0.5;
                        pillarHeight = std::min<int64_t>(
                            feature.maxHeight,
                            pillarHeight + static_cast<int64_t>(
                                std::floor(t * static_cast<double>(
                                    heightRange + 1)))
                        );
                    }

                    const int64_t chunkMinY =
                        static_cast<int64_t>(ctx.coord.y) * Chunk::SIZE;
                    const int64_t chunkMaxY = chunkMinY + Chunk::SIZE - 1;
                    const int64_t firstWorldY = std::max<int64_t>(
                        static_cast<int64_t>(height) + 1, chunkMinY);
                    const int64_t lastWorldY = std::min<int64_t>(
                        static_cast<int64_t>(height) + pillarHeight, chunkMaxY);
                    for (int64_t worldY = firstWorldY;
                         worldY <= lastWorldY; ++worldY) {
                        const int localY = static_cast<int>(worldY - chunkMinY);
                        BlockState state;
                        state.id = feature.block;
                        buffer.at(x, localY, z) = state;
                    }
                }
            }
        }
    }

private:
    struct FeatureResolved {
        std::string name;
        BlockID block;
        float chance = 0.0f;
        int minHeight = 1;
        int maxHeight = 3;
        uint32_t seed = 0;
        std::vector<int> biomeIndices;
    };

    const GeneratorDefinitionData& m_definition;
    std::vector<FeatureResolved> m_features;
};

DensityGraph validateAndBuildDensityGraph(const BlockRegistry& registry,
                                          const GeneratorDefinitionData& config) {
    validateGeneratorDefinitionContent(
        config, registry, "WorldGenerator definition");
    DensityGraph densityGraph;
    std::string graphError;
    if (!buildDensityGraph(config, densityGraph, graphError)) {
        throw std::runtime_error(
            "WorldGenerator: invalid density graph: " + graphError);
    }
    return densityGraph;
}

std::vector<std::unique_ptr<const WorldGenStage>> buildStages(
    const GeneratorDefinitionData& config,
    uint32_t seed,
    const BlockRegistry& registry,
    const DensityGraph& densityGraph) {
    std::vector<std::unique_ptr<const WorldGenStage>> stages;
    stages.reserve(kWorldGenPipelineStages.size());
    stages.push_back(std::make_unique<ClimateGlobalStage>(config, seed));
    stages.push_back(std::make_unique<ClimateLocalStage>(config, seed));
    stages.push_back(std::make_unique<BiomeResolveStage>(config));
    stages.push_back(std::make_unique<TerrainDensityStage>(
        config, seed, &densityGraph));
    if (config.caves.enabled) {
        stages.push_back(std::make_unique<CavesStage>(
            config, seed, &densityGraph));
    }
    stages.push_back(std::make_unique<SurfaceRulesStage>(
        config, seed, registry, &densityGraph));
    if (config.structures.enabled) {
        stages.push_back(std::make_unique<StructuresStage>(
            config, seed, registry));
    }
    spdlog::debug("WorldGenerator built {} stages", stages.size());
    return stages;
}

} // namespace

GeneratorDefinitionData validateGeneratorDefinitionData(
    const BlockRegistry& registry,
    GeneratorDefinitionData definition) {
    validateGeneratorDefinitionContent(
        definition, registry, "WorldGenerator definition");
    return definition;
}

uint32_t validateSemanticsVersion(uint32_t semanticsVersion) {
    if (semanticsVersion == 0) {
        throw std::invalid_argument(
            "WorldGenerator requires a non-zero generator semantics version");
    }
    return semanticsVersion;
}

BlockID resolveRequiredBlock(const BlockRegistry& registry,
                             const std::string& identifier,
                             std::string_view role) {
    const auto block = registry.findByIdentifier(identifier);
    if (!block) {
        throw std::invalid_argument(
            "WorldGenerator required " + std::string(role) + " material '" +
            identifier + "' is unavailable");
    }
    return *block;
}

WorldGenerator::WorldGenerator(const BlockRegistry& registry,
                               GeneratorDefinitionData definition,
                               uint32_t seed,
                               uint32_t semanticsVersion,
                               std::shared_ptr<const BlockGalleryChunkGenerator>
                                   blockGallery)
    : m_registry(registry),
      m_definition(validateGeneratorDefinitionData(
          registry, std::move(definition))),
      m_seed(seed),
      m_semanticsVersion(validateSemanticsVersion(semanticsVersion)),
      m_solidBlock(resolveRequiredBlock(
          registry, m_definition.terrain.solidMaterial, "solid")),
      m_waterBlock(resolveRequiredBlock(
          registry, m_definition.terrain.waterMaterial, "water")),
      m_densityGraph(validateAndBuildDensityGraph(m_registry, m_definition)),
      m_stages(buildStages(
          m_definition, m_seed, m_registry, m_densityGraph)),
      m_blockGallery(std::move(blockGallery)) {
    if (m_blockGallery) {
        m_blockGallery->validateGeneratorBounds(m_definition.bounds);
    }
}

bool WorldGenerator::matchesGenerationInputs(
    const GeneratorDefinitionData& definition,
    uint32_t seed,
    uint32_t semanticsVersion) const {
    return m_seed == seed &&
        m_semanticsVersion == semanticsVersion &&
        m_definition == definition;
}

bool WorldGenerator::matchesRuntimeGenerator(
    const WorldGenerator& other) const {
    if (!matchesGenerationInputs(
            other.definition(),
            other.seed(),
            other.semanticsVersion())) {
        return false;
    }
    if (m_blockGallery == other.m_blockGallery) {
        return true;
    }
    if (!m_blockGallery || !other.m_blockGallery) {
        return false;
    }
    return m_blockGallery->matchesRuntimeBehavior(*other.m_blockGallery);
}

bool WorldGenerator::shouldPersistGeneratedChunk(ChunkCoord coord) const {
    return m_blockGallery && m_blockGallery->containsChunk(coord);
}

void WorldGenerator::generate(ChunkCoord coord, ChunkBuffer& out,
                              const std::atomic_bool* cancel) const {
    const LocalWorldYRange worldYRange =
        localWorldYRange(coord, m_definition.bounds);
    if (worldYRange.empty()) {
        out.blocks.fill(BlockState{});
        return;
    }
    if (m_blockGallery) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            out.blocks.fill(BlockState{});
            return;
        }
        m_blockGallery->generate(coord, out);
        return;
    }
    requireSupportedChunkCoordinates(
        coord, m_definition.structures.enabled);
    clearOutsideWorldYRange(out, worldYRange);

    WorldGenContext ctx;
    ctx.coord = coord;
    ctx.definition = &m_definition;
    ctx.cancel = cancel;
    ctx.solidBlock = m_solidBlock;
    ctx.waterBlock = m_waterBlock;

    for (const auto& stage : m_stages) {
        if (ctx.shouldCancel()) {
            break;
        }
        stage->apply(ctx, out);
        if (ctx.shouldCancel()) {
            break;
        }
    }
    clearOutsideWorldYRange(out, worldYRange);
}

} // namespace Rigel::Voxel
