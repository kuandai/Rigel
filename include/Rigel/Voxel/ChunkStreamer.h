#pragma once

#include "ChunkCache.h"
#include "ChunkBenchmark.h"
#include "ChunkManager.h"
#include "ChunkRenderer.h"
#include "WorldGenConfig.h"
#include "WorldGenerator.h"

#include <glm/vec3.hpp>
#include <memory>

namespace Rigel::Voxel {

class ChunkStreamer {
public:
    void setConfig(const WorldGenConfig::StreamConfig& config);
    void bind(ChunkManager* manager,
              ChunkRenderer* renderer,
              std::shared_ptr<WorldGenerator> generator);
    void setBenchmark(ChunkBenchmarkStats* stats);

    void update(const glm::vec3& cameraPos);

private:
    WorldGenConfig::StreamConfig m_config;
    ChunkManager* m_chunkManager = nullptr;
    ChunkRenderer* m_renderer = nullptr;
    std::shared_ptr<WorldGenerator> m_generator;
    ChunkCache m_cache;
    ChunkBenchmarkStats* m_benchmark = nullptr;

    ChunkCoord cameraToChunk(const glm::vec3& cameraPos) const;
};

} // namespace Rigel::Voxel
