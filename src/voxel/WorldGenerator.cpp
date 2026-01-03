#include "Rigel/Voxel/WorldGenerator.h"

#include "Rigel/Voxel/DensityFunction.h"
#include "Rigel/Voxel/Noise.h"
#include "Rigel/Voxel/WorldGenStages.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {
constexpr const char* kDefaultWaterBlock = "rigel:water";
constexpr const char* kDefaultSandBlock = "rigel:sand";
constexpr int kClimateColumnCount = Chunk::SIZE * Chunk::SIZE;
constexpr int kDefaultNoiseSampleStep = 4;
static_assert(Chunk::SIZE % kDefaultNoiseSampleStep == 0,
              "Chunk size must be divisible by default noise sample step.");

int columnIndex(int x, int z) {
    return x + z * Chunk::SIZE;
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
               const WorldGenConfig::NoiseConfig& config) {
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
        int originX = coord.x * Chunk::SIZE;
        int originY = coord.y * Chunk::SIZE;
        int originZ = coord.z * Chunk::SIZE;
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

class NoopStage : public WorldGenStage {
public:
    explicit NoopStage(const char* stageName)
        : m_name(stageName)
    {}

    const char* name() const override { return m_name; }
    void apply(WorldGenContext&, ChunkBuffer&) override {}

private:
    const char* m_name;
};

class ClimateGlobalStage : public WorldGenStage {
public:
    explicit ClimateGlobalStage(const WorldGenConfig& config)
        : m_config(config)
        , m_temperatureSeed(Noise::seedForChannel(config.seed, "climate_global/temperature"))
        , m_humiditySeed(Noise::seedForChannel(config.seed, "climate_global/humidity"))
        , m_continentalnessSeed(Noise::seedForChannel(config.seed, "climate_global/continentalness"))
    {}

    const char* name() const override { return "climate_global"; }

    void apply(WorldGenContext& ctx, ChunkBuffer&) override {
        const auto& climate = m_config.climate;
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
    const WorldGenConfig& m_config;
    uint32_t m_temperatureSeed = 0;
    uint32_t m_humiditySeed = 0;
    uint32_t m_continentalnessSeed = 0;
};

class ClimateLocalStage : public WorldGenStage {
public:
    explicit ClimateLocalStage(const WorldGenConfig& config)
        : m_config(config)
        , m_temperatureSeed(Noise::seedForChannel(config.seed, "climate_local/temperature"))
        , m_humiditySeed(Noise::seedForChannel(config.seed, "climate_local/humidity"))
        , m_continentalnessSeed(Noise::seedForChannel(config.seed, "climate_local/continentalness"))
    {}

    const char* name() const override { return "climate_local"; }

    void apply(WorldGenContext& ctx, ChunkBuffer&) override {
        const auto& climate = m_config.climate;
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
    const WorldGenConfig& m_config;
    uint32_t m_temperatureSeed = 0;
    uint32_t m_humiditySeed = 0;
    uint32_t m_continentalnessSeed = 0;
};

class BiomeResolveStage : public WorldGenStage {
public:
    explicit BiomeResolveStage(const WorldGenConfig& config)
        : m_config(config)
    {
        const auto& band = m_config.biomes.coastBand;
        if (band.enabled && !band.biome.empty()) {
            m_coastMin = band.minContinentalness;
            m_coastMax = band.maxContinentalness;
            if (m_coastMin > m_coastMax) {
                std::swap(m_coastMin, m_coastMax);
            }
            for (size_t i = 0; i < m_config.biomes.entries.size(); ++i) {
                if (m_config.biomes.entries[i].name == band.biome) {
                    m_coastBiomeIndex = static_cast<int>(i);
                    break;
                }
            }
            m_coastBandEnabled = (m_coastBiomeIndex >= 0);
        }
    }

    const char* name() const override { return "biome_resolve"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        (void)buffer;
        const auto& biomes = m_config.biomes;
        if (biomes.entries.empty()) {
            for (int i = 0; i < kClimateColumnCount; ++i) {
                ctx.biomes[static_cast<size_t>(i)] = {};
            }
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
                int index = columnIndex(x, z);
                const ClimateSample& sample = ctx.climate[index];

                int bestIndex = -1;
                int secondIndex = -1;
                float bestWeight = 0.0f;
                float secondWeight = 0.0f;
                bool coastActive = m_coastBandEnabled
                    && sample.continentalness >= m_coastMin
                    && sample.continentalness <= m_coastMax;

                for (size_t i = 0; i < biomes.entries.size(); ++i) {
                    if (!coastActive && m_coastBandEnabled &&
                        static_cast<int>(i) == m_coastBiomeIndex) {
                        continue;
                    }
                    const auto& biome = biomes.entries[i];
                    float dt = sample.temperature - biome.target.temperature;
                    float dh = sample.humidity - biome.target.humidity;
                    float dc = sample.continentalness - biome.target.continentalness;
                    float dist = std::sqrt(dt * dt + dh * dh + dc * dc);
                    float weight = biome.weight
                        / std::pow(dist + biomes.blend.epsilon, biomes.blend.blendPower);
                    if (weight > bestWeight) {
                        secondWeight = bestWeight;
                        secondIndex = bestIndex;
                        bestWeight = weight;
                        bestIndex = static_cast<int>(i);
                    } else if (weight > secondWeight) {
                        secondWeight = weight;
                        secondIndex = static_cast<int>(i);
                    }
                }

                if (coastActive && m_coastBiomeIndex >= 0) {
                    secondIndex = bestIndex;
                    bestIndex = m_coastBiomeIndex;
                    bestWeight = 1.0f;
                    secondWeight = 0.0f;
                }

                BiomeSample result;
                result.primary = bestIndex;
                result.secondary = secondIndex;
                float total = bestWeight + secondWeight;
                result.blend = (total > 0.0f) ? (secondWeight / total) : 0.0f;
                ctx.biomes[static_cast<size_t>(index)] = result;
            }
        }
    }

private:
    const WorldGenConfig& m_config;
    int m_coastBiomeIndex = -1;
    float m_coastMin = 0.0f;
    float m_coastMax = 0.0f;
    bool m_coastBandEnabled = false;
};

class TerrainDensityStage : public WorldGenStage {
public:
    TerrainDensityStage(const WorldGenConfig& config,
                        const DensityGraph* graph)
        : m_config(config)
        , m_graph(graph)
    {
        for (size_t i = 0; i < m_config.biomes.entries.size(); ++i) {
            const auto& biome = m_config.biomes.entries[i];
            if (biome.name == "sea") {
                m_seaBiomeIndex = static_cast<int>(i);
            } else if (biome.name == "beach") {
                m_beachBiomeIndex = static_cast<int>(i);
            }
        }
    }

    const char* name() const override { return "terrain_density"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        const auto& terrain = m_config.terrain;
        const auto& world = m_config.world;

        buffer.blocks.fill(BlockState{});
        DensityEvaluator evaluator(m_graph, m_config.seed);
        NoiseGridCache noiseCache(m_graph, m_config.seed, ctx.coord);
        NoiseGrid fallbackNoise;
        if (!m_graph || m_graph->empty()) {
            int originX = ctx.coord.x * Chunk::SIZE;
            int originY = ctx.coord.y * Chunk::SIZE;
            int originZ = ctx.coord.z * Chunk::SIZE;
            fallbackNoise.build(originX, originY, originZ,
                                m_config.seed ^ 0x9e3779b9u,
                                kDefaultNoiseSampleStep,
                                terrain.densityNoise);
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
                int index = columnIndex(x, z);
                int biomeIndex = ctx.biomes[static_cast<size_t>(index)].primary;
                bool allowWater = (biomeIndex == m_seaBiomeIndex) || (biomeIndex == m_beachBiomeIndex);
                int maxSolid = world.minY - 1;

                for (int y = 0; y < Chunk::SIZE; ++y) {
                    int worldY = ctx.coord.y * Chunk::SIZE + y;
                    bool solid = false;
                    if (m_graph && !m_graph->empty()) {
                        DensitySampleContext sampleCtx{
                            .worldX = worldX,
                            .worldY = worldY,
                            .worldZ = worldZ,
                            .climate = &ctx.climate[static_cast<size_t>(index)],
                            .noiseCache = &noiseCache
                        };
                        evaluator.beginSample();
                        float density = evaluator.evaluateOutput("base_density", sampleCtx);
                        solid = density >= 0.0f;
                    } else {
                        float noise = Noise::fbm2D(
                            static_cast<float>(worldX),
                            static_cast<float>(worldZ),
                            m_config.seed,
                            terrain.heightNoise
                        );
                        float heightF = terrain.baseHeight + noise * terrain.heightVariation;
                        float densityNoise = fallbackNoise.valid
                            ? fallbackNoise.sample(worldX, worldY, worldZ)
                            : Noise::fbm3D(
                                static_cast<float>(worldX),
                                static_cast<float>(worldY),
                                static_cast<float>(worldZ),
                                m_config.seed ^ 0x9e3779b9u,
                                terrain.densityNoise
                            );
                        float gradient = (heightF - static_cast<float>(worldY)) * terrain.gradientStrength;
                        float density = densityNoise * terrain.densityStrength + gradient;
                        solid = density >= 0.0f;
                    }

                    if (solid) {
                        BlockState state;
                        state.id = ctx.solidBlock;
                        buffer.at(x, y, z) = state;
                        if (worldY > maxSolid) {
                            maxSolid = worldY;
                        }
                    } else if (allowWater && worldY <= world.seaLevel && !ctx.waterBlock.isAir()) {
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
    const WorldGenConfig& m_config;
    const DensityGraph* m_graph = nullptr;
    int m_seaBiomeIndex = -1;
    int m_beachBiomeIndex = -1;
};

class CavesStage : public WorldGenStage {
public:
    CavesStage(const WorldGenConfig& config,
               const DensityGraph* graph)
        : m_config(config)
        , m_graph(graph)
    {}

    const char* name() const override { return "caves"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        const auto& caves = m_config.caves;
        if (!caves.enabled || !m_graph || m_graph->empty()) {
            return;
        }
        const auto& world = m_config.world;

        DensityEvaluator evaluator(m_graph, m_config.seed);
        NoiseGridCache noiseCache(m_graph, m_config.seed, ctx.coord);

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
    const WorldGenConfig& m_config;
    const DensityGraph* m_graph = nullptr;
};

class SurfaceRulesStage : public WorldGenStage {
public:
    SurfaceRulesStage(const WorldGenConfig& config,
                      const BlockRegistry& registry,
                      const DensityGraph* graph)
        : m_config(config)
        , m_graph(graph) {
        m_surfaceByBiome.reserve(config.biomes.entries.size());
        for (const auto& biome : config.biomes.entries) {
            std::vector<ResolvedLayer> layers;
            for (const auto& layer : biome.surface) {
                auto blockId = registry.findByIdentifier(layer.block);
                if (blockId) {
                    layers.push_back({*blockId, layer.depth});
                }
            }
            m_surfaceByBiome.push_back(std::move(layers));
        }
    }

    const char* name() const override { return "surface_rules"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        const auto& terrain = m_config.terrain;
        const auto& world = m_config.world;
        if (ctx.surfaceBlock.isAir() && ctx.sandBlock.isAir() && m_surfaceByBiome.empty()) {
            return;
        }

        DensityEvaluator evaluator(m_graph, m_config.seed);
        NoiseGridCache noiseCache(m_graph, m_config.seed, ctx.coord);
        NoiseGrid fallbackNoise;
        bool useGraph = (m_graph && !m_graph->empty());
        if (!useGraph) {
            int originX = ctx.coord.x * Chunk::SIZE;
            int originY = ctx.coord.y * Chunk::SIZE;
            int originZ = ctx.coord.z * Chunk::SIZE;
            fallbackNoise.build(originX, originY, originZ,
                                m_config.seed ^ 0x9e3779b9u,
                                kDefaultNoiseSampleStep,
                                terrain.densityNoise);
        }

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
                    ctx, x, z, useGraph, evaluator, noiseCache, fallbackNoise
                );
                ctx.heightMap[index] = height;
                if (height < world.minY) {
                    continue;
                }
                int localY = height - ctx.coord.y * Chunk::SIZE;
                if (localY < 0 || localY >= Chunk::SIZE) {
                    continue;
                }

                const std::vector<ResolvedLayer>* layers = nullptr;
                int biomeIndex = ctx.biomes[index].primary;
                if (biomeIndex >= 0 && biomeIndex < static_cast<int>(m_surfaceByBiome.size())) {
                    layers = &m_surfaceByBiome[static_cast<size_t>(biomeIndex)];
                }

                if (!layers || layers->empty()) {
                    BlockID surfaceId = ctx.surfaceBlock;
                    if (height <= world.seaLevel + 4 && !ctx.sandBlock.isAir()) {
                        surfaceId = ctx.sandBlock;
                    }
                    applyLayer(buffer, ctx, x, z, height, surfaceId, terrain.surfaceDepth);
                    continue;
                }

                int depthOffset = 0;
                for (const auto& layer : *layers) {
                    if (layer.depth <= 0) {
                        continue;
                    }
                    for (int d = 0; d < layer.depth; ++d) {
                        int worldY = height - depthOffset;
                        if (worldY < world.minY) {
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

    void applyLayer(ChunkBuffer& buffer, WorldGenContext& ctx, int x, int z,
                    int height, BlockID block, int depth) const {
        if (depth <= 0 || block.isAir()) {
            return;
        }
        for (int d = 0; d < depth; ++d) {
            int worldY = height - d;
            int localY = worldY - ctx.coord.y * Chunk::SIZE;
            if (localY < 0 || localY >= Chunk::SIZE) {
                continue;
            }
            BlockState state;
            state.id = block;
            buffer.at(x, localY, z) = state;
        }
    }

    int findSurfaceHeightGlobal(const WorldGenContext& ctx, int x, int z,
                                bool useGraph,
                                DensityEvaluator& evaluator,
                                const NoiseGridCache& noiseCache,
                                const NoiseGrid& fallbackNoise) const {
        const auto& world = m_config.world;
        int worldX = ctx.coord.x * Chunk::SIZE + x;
        int worldZ = ctx.coord.z * Chunk::SIZE + z;
        for (int worldY = world.maxY; worldY >= world.minY; --worldY) {
            if (isSolidAt(ctx, worldX, worldY, worldZ,
                          useGraph, evaluator, noiseCache, fallbackNoise)) {
                return worldY;
            }
        }
        return world.minY - 1;
    }

    bool isSolidAt(const WorldGenContext& ctx,
                   int worldX,
                   int worldY,
                   int worldZ,
                   bool useGraph,
                   DensityEvaluator& evaluator,
                   const NoiseGridCache& noiseCache,
                   const NoiseGrid& fallbackNoise) const {
        const auto& world = m_config.world;
        if (worldY < world.minY) {
            return false;
        }
        if (useGraph) {
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
            float density = evaluator.evaluateOutput("base_density", sampleCtx);
            if (density < 0.0f) {
                return false;
            }
            if (m_config.caves.enabled) {
                float caveDensity = evaluator.evaluateOutput(m_config.caves.densityOutput, sampleCtx);
                if (caveDensity > m_config.caves.threshold) {
                    return false;
                }
            }
            return true;
        }

        const auto& terrain = m_config.terrain;
        float noise = Noise::fbm2D(
            static_cast<float>(worldX),
            static_cast<float>(worldZ),
            m_config.seed,
            terrain.heightNoise
        );
        float heightF = terrain.baseHeight + noise * terrain.heightVariation;
        float densityNoise = fallbackNoise.valid
            ? fallbackNoise.sample(worldX, worldY, worldZ)
            : Noise::fbm3D(
                static_cast<float>(worldX),
                static_cast<float>(worldY),
                static_cast<float>(worldZ),
                m_config.seed ^ 0x9e3779b9u,
                terrain.densityNoise
            );
        float gradient = (heightF - static_cast<float>(worldY)) * terrain.gradientStrength;
        float density = densityNoise * terrain.densityStrength + gradient;
        return density >= 0.0f;
    }

    const WorldGenConfig& m_config;
    const DensityGraph* m_graph = nullptr;
    std::vector<std::vector<ResolvedLayer>> m_surfaceByBiome;
};

class StructuresStage : public WorldGenStage {
public:
    StructuresStage(const WorldGenConfig& config, const BlockRegistry& registry)
        : m_config(config) {
        for (const auto& feature : config.structures.features) {
            auto blockId = registry.findByIdentifier(feature.block);
            if (!blockId) {
                continue;
            }
            FeatureResolved resolved;
            resolved.name = feature.name;
            resolved.block = *blockId;
            resolved.chance = feature.chance;
            resolved.minHeight = feature.minHeight;
            resolved.maxHeight = std::max(feature.minHeight, feature.maxHeight);
            resolved.seed = Noise::seedForChannel(config.seed, "feature/" + feature.name);
            if (!feature.biomes.empty()) {
                for (const auto& biomeName : feature.biomes) {
                    for (size_t i = 0; i < config.biomes.entries.size(); ++i) {
                        if (config.biomes.entries[i].name == biomeName) {
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

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        if (m_features.empty()) {
            return;
        }
        const auto& world = m_config.world;

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
                if (height < world.minY) {
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
                    int heightRange = feature.maxHeight - feature.minHeight;
                    int pillarHeight = feature.minHeight;
                    if (heightRange > 0) {
                        float t = (heightNoise + 1.0f) * 0.5f;
                        pillarHeight += static_cast<int>(std::floor(t * (heightRange + 1)));
                    }

                    for (int h = 1; h <= pillarHeight; ++h) {
                        int worldY = height + h;
                        int localY = worldY - ctx.coord.y * Chunk::SIZE;
                        if (localY < 0 || localY >= Chunk::SIZE) {
                            continue;
                        }
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

    const WorldGenConfig& m_config;
    std::vector<FeatureResolved> m_features;
};

} // namespace

WorldGenerator::WorldGenerator(const BlockRegistry& registry)
    : m_registry(registry)
{
    registerDefaultStages();
    rebuildStages();
}

void WorldGenerator::setConfig(WorldGenConfig config) {
    m_config = std::move(config);
    std::string graphError;
    if (!buildDensityGraph(m_config, m_densityGraph, graphError) && !graphError.empty()) {
        spdlog::warn("WorldGenerator: density graph issue: {}", graphError);
    }
    rebuildStages();
}

void WorldGenerator::generate(ChunkCoord coord, ChunkBuffer& out,
                              const std::atomic_bool* cancel) const {
    WorldGenContext ctx;
    ctx.coord = coord;
    ctx.config = &m_config;
    ctx.registry = &m_registry;
    ctx.cancel = cancel;

    auto solidId = m_registry.findByIdentifier(m_config.solidBlock);
    ctx.solidBlock = solidId.value_or(BlockRegistry::airId());

    auto surfaceId = m_registry.findByIdentifier(m_config.surfaceBlock);
    ctx.surfaceBlock = surfaceId.value_or(BlockRegistry::airId());

    auto waterId = m_registry.findByIdentifier(kDefaultWaterBlock);
    ctx.waterBlock = waterId.value_or(BlockRegistry::airId());

    auto sandId = m_registry.findByIdentifier(kDefaultSandBlock);
    ctx.sandBlock = sandId.value_or(BlockRegistry::airId());

    if (ctx.solidBlock.isAir() || ctx.surfaceBlock.isAir() ||
        ctx.waterBlock.isAir() || ctx.sandBlock.isAir()) {
        static bool warned = false;
        if (!warned) {
            if (ctx.solidBlock.isAir()) {
                spdlog::warn("WorldGenerator: solid block '{}' not found, using air", m_config.solidBlock);
            }
            if (ctx.surfaceBlock.isAir()) {
                spdlog::warn("WorldGenerator: surface block '{}' not found, using air", m_config.surfaceBlock);
            }
            if (ctx.waterBlock.isAir()) {
                spdlog::warn("WorldGenerator: water block '{}' not found, using air", kDefaultWaterBlock);
            }
            if (ctx.sandBlock.isAir()) {
                spdlog::warn("WorldGenerator: sand block '{}' not found, using air", kDefaultSandBlock);
            }
            warned = true;
        }
    }

    for (const auto& stage : m_stages) {
        if (ctx.shouldCancel()) {
            return;
        }
        stage->apply(ctx, out);
        if (ctx.shouldCancel()) {
            return;
        }
    }
}

void WorldGenerator::registerDefaultStages() {
    m_stageFactories["climate_global"] = [this]() {
        return std::make_unique<ClimateGlobalStage>(m_config);
    };
    m_stageFactories["climate_local"] = [this]() {
        return std::make_unique<ClimateLocalStage>(m_config);
    };
    m_stageFactories["biome_resolve"] = [this]() {
        return std::make_unique<BiomeResolveStage>(m_config);
    };
    m_stageFactories["terrain_density"] = [this]() {
        return std::make_unique<TerrainDensityStage>(m_config, &m_densityGraph);
    };
    m_stageFactories["caves"] = [this]() {
        return std::make_unique<CavesStage>(m_config, &m_densityGraph);
    };
    m_stageFactories["surface_rules"] = [this]() {
        return std::make_unique<SurfaceRulesStage>(m_config, m_registry, &m_densityGraph);
    };
    m_stageFactories["structures"] = [this]() {
        return std::make_unique<StructuresStage>(m_config, m_registry);
    };
    m_stageFactories["post_process"] = []() {
        return std::make_unique<NoopStage>("post_process");
    };
}

void WorldGenerator::rebuildStages() {
    m_stages.clear();

    for (const char* stageName : kWorldGenPipelineStages) {
        if (!isStageEnabled(stageName)) {
            continue;
        }
        auto it = m_stageFactories.find(stageName);
        if (it == m_stageFactories.end()) {
            m_stages.push_back(std::make_unique<NoopStage>(stageName));
            continue;
        }
        m_stages.push_back(it->second());
    }

    spdlog::debug("WorldGenerator built {} stages", m_stages.size());
}

bool WorldGenerator::isStageEnabled(const std::string& stage) const {
    return m_config.isStageEnabled(stage);
}

} // namespace Rigel::Voxel
