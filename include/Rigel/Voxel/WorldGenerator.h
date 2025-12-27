#pragma once

#include "Block.h"
#include "BlockRegistry.h"
#include "Chunk.h"
#include "ChunkCoord.h"
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

struct WorldGenContext {
    ChunkCoord coord;
    const WorldGenConfig* config = nullptr;
    BlockID solidBlock = BlockRegistry::airId();
    BlockID surfaceBlock = BlockRegistry::airId();
    BlockID waterBlock = BlockRegistry::airId();
    BlockID sandBlock = BlockRegistry::airId();
    std::array<int, Chunk::SIZE * Chunk::SIZE> heightMap{};
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
    std::vector<std::unique_ptr<WorldGenStage>> m_stages;
    std::unordered_map<std::string, StageFactory> m_stageFactories;

    void registerDefaultStages();
    void rebuildStages();
    bool isStageEnabled(const std::string& stage) const;
};

} // namespace Rigel::Voxel
