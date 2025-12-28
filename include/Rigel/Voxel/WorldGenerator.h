#pragma once

#include "Block.h"
#include "BlockRegistry.h"
#include "Chunk.h"
#include "ChunkCoord.h"
#include "DensityFunction.h"
#include "WorldGenConfig.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rigel::Voxel {

struct ChunkBuffer {
    std::array<BlockState, Chunk::VOLUME> blocks{};

    BlockState& at(int x, int y, int z) {
        return blocks[x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE];
    }
    const BlockState& at(int x, int y, int z) const {
        return blocks[x + y * Chunk::SIZE + z * Chunk::SIZE * Chunk::SIZE];
    }
};

struct ClimateSample {
    float temperature = 0.0f;
    float humidity = 0.0f;
    float continentalness = 0.0f;
};

struct BiomeSample {
    int primary = -1;
    int secondary = -1;
    float blend = 0.0f;
};

struct WorldGenContext {
    ChunkCoord coord;
    const WorldGenConfig* config = nullptr;
    const BlockRegistry* registry = nullptr;
    BlockID solidBlock = BlockRegistry::airId();
    BlockID surfaceBlock = BlockRegistry::airId();
    BlockID waterBlock = BlockRegistry::airId();
    BlockID sandBlock = BlockRegistry::airId();
    std::array<int, Chunk::SIZE * Chunk::SIZE> heightMap{};
    std::array<ClimateSample, Chunk::SIZE * Chunk::SIZE> climate{};
    std::array<BiomeSample, Chunk::SIZE * Chunk::SIZE> biomes{};
    const std::atomic_bool* cancel = nullptr;

    bool shouldCancel() const {
        return cancel && cancel->load(std::memory_order_relaxed);
    }

};

class WorldGenStage {
public:
    virtual ~WorldGenStage() = default;
    virtual const char* name() const = 0;
    virtual void apply(WorldGenContext& ctx, ChunkBuffer& buffer) = 0;
};

class WorldGenerator {
public:
    using StageFactory = std::function<std::unique_ptr<WorldGenStage>()>;

    explicit WorldGenerator(const BlockRegistry& registry);

    void setConfig(WorldGenConfig config);
    const WorldGenConfig& config() const { return m_config; }

    void generate(ChunkCoord coord, ChunkBuffer& out,
                  const std::atomic_bool* cancel = nullptr) const;

private:
    const BlockRegistry& m_registry;
    WorldGenConfig m_config;
    DensityGraph m_densityGraph;
    std::vector<std::unique_ptr<WorldGenStage>> m_stages;
    std::unordered_map<std::string, StageFactory> m_stageFactories;

    void registerDefaultStages();
    void rebuildStages();
    bool isStageEnabled(const std::string& stage) const;
};

} // namespace Rigel::Voxel
