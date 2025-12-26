#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {
constexpr const char* kPipelineStages[] = {
    "climate_global",
    "climate_local",
    "biome_resolve",
    "terrain_density",
    "caves",
    "surface_rules",
    "structures",
    "post_process"
};
constexpr const char* kDefaultWaterBlock = "rigel:water";

uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

float hashToUnit(uint64_t x) {
    uint64_t h = hash64(x);
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float valueNoise2D(int x, int z, uint32_t seed) {
    uint64_t key = (static_cast<uint64_t>(x) << 32) ^ static_cast<uint32_t>(z) ^ seed;
    return hashToUnit(key) * 2.0f - 1.0f;
}

float noise2D(float x, float z, uint32_t seed) {
    int x0 = static_cast<int>(std::floor(x));
    int z0 = static_cast<int>(std::floor(z));
    int x1 = x0 + 1;
    int z1 = z0 + 1;

    float fx = x - static_cast<float>(x0);
    float fz = z - static_cast<float>(z0);

    float v00 = valueNoise2D(x0, z0, seed);
    float v10 = valueNoise2D(x1, z0, seed);
    float v01 = valueNoise2D(x0, z1, seed);
    float v11 = valueNoise2D(x1, z1, seed);

    float tx = smoothstep(fx);
    float tz = smoothstep(fz);
    float a = lerp(v00, v10, tx);
    float b = lerp(v01, v11, tx);
    return lerp(a, b, tz);
}

float fbm2D(float x, float z, uint32_t seed, const WorldGenConfig::NoiseConfig& config) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = config.frequency;
    float maxValue = 0.0f;

    for (int i = 0; i < config.octaves; ++i) {
        total += noise2D(x * frequency, z * frequency, seed + static_cast<uint32_t>(i)) * amplitude;
        maxValue += amplitude;
        amplitude *= config.persistence;
        frequency *= config.lacunarity;
    }

    if (maxValue > 0.0f) {
        total /= maxValue;
    }

    return total;
}

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

class TerrainDensityStage : public WorldGenStage {
public:
    const char* name() const override { return "terrain_density"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        const auto& config = *ctx.config;
        const auto& terrain = config.terrain;

        if (ctx.coord.y < 0 && !ctx.waterBlock.isAir()) {
            BlockState state;
            state.id = ctx.waterBlock;
            buffer.blocks.fill(state);
            return;
        }

        buffer.blocks.fill(BlockState{});

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
                float noise = fbm2D(static_cast<float>(worldX),
                                    static_cast<float>(worldZ),
                                    config.seed,
                                    terrain.heightNoise);
                float heightF = terrain.baseHeight + noise * terrain.heightVariation;
                int height = static_cast<int>(std::floor(heightF));
                ctx.heightMap[x + z * Chunk::SIZE] = height;

                for (int y = 0; y < Chunk::SIZE; ++y) {
                    int worldY = ctx.coord.y * Chunk::SIZE + y;
                    if (worldY <= height) {
                        BlockState state;
                        state.id = ctx.solidBlock;
                        buffer.at(x, y, z) = state;
                    }
                }
            }
        }
    }
};

class SurfaceRulesStage : public WorldGenStage {
public:
    const char* name() const override { return "surface_rules"; }

    void apply(WorldGenContext& ctx, ChunkBuffer& buffer) override {
        const auto& terrain = ctx.config->terrain;
        if (ctx.surfaceBlock.isAir()) {
            return;
        }
        if (ctx.coord.y < 0 && !ctx.waterBlock.isAir()) {
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
                int height = ctx.heightMap[x + z * Chunk::SIZE];
                for (int y = 0; y < Chunk::SIZE; ++y) {
                    int worldY = ctx.coord.y * Chunk::SIZE + y;
                    if (worldY <= height && worldY > height - terrain.surfaceDepth) {
                        BlockState state;
                        state.id = ctx.surfaceBlock;
                        buffer.at(x, y, z) = state;
                    }
                }
            }
        }
    }
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
    rebuildStages();
}

void WorldGenerator::generate(ChunkCoord coord, ChunkBuffer& out,
                              const std::atomic_bool* cancel) const {
    WorldGenContext ctx;
    ctx.coord = coord;
    ctx.config = &m_config;
    ctx.cancel = cancel;

    auto solidId = m_registry.findByIdentifier(m_config.solidBlock);
    ctx.solidBlock = solidId.value_or(BlockRegistry::airId());

    auto surfaceId = m_registry.findByIdentifier(m_config.surfaceBlock);
    ctx.surfaceBlock = surfaceId.value_or(BlockRegistry::airId());

    auto waterId = m_registry.findByIdentifier(kDefaultWaterBlock);
    ctx.waterBlock = waterId.value_or(BlockRegistry::airId());

    if (ctx.solidBlock.isAir() || ctx.surfaceBlock.isAir() || ctx.waterBlock.isAir()) {
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
    m_stageFactories["climate_global"] = []() {
        return std::make_unique<NoopStage>("climate_global");
    };
    m_stageFactories["climate_local"] = []() {
        return std::make_unique<NoopStage>("climate_local");
    };
    m_stageFactories["biome_resolve"] = []() {
        return std::make_unique<NoopStage>("biome_resolve");
    };
    m_stageFactories["terrain_density"] = []() {
        return std::make_unique<TerrainDensityStage>();
    };
    m_stageFactories["caves"] = []() {
        return std::make_unique<NoopStage>("caves");
    };
    m_stageFactories["surface_rules"] = []() {
        return std::make_unique<SurfaceRulesStage>();
    };
    m_stageFactories["structures"] = []() {
        return std::make_unique<NoopStage>("structures");
    };
    m_stageFactories["post_process"] = []() {
        return std::make_unique<NoopStage>("post_process");
    };
}

void WorldGenerator::rebuildStages() {
    m_stages.clear();

    for (const char* stageName : kPipelineStages) {
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
