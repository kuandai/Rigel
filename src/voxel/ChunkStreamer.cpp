#include "Rigel/Voxel/ChunkStreamer.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_set>

namespace Rigel::Voxel {

namespace {
int distanceSquared(const ChunkCoord& a, const ChunkCoord& b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    int dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
} // namespace

void ChunkStreamer::setConfig(const WorldGenConfig::StreamConfig& config) {
    m_config = config;
    m_cache.setMaxChunks(m_config.maxResidentChunks);
}

void ChunkStreamer::bind(ChunkManager* manager,
                         ChunkRenderer* renderer,
                         std::shared_ptr<WorldGenerator> generator) {
    m_chunkManager = manager;
    m_renderer = renderer;
    m_generator = std::move(generator);
}

void ChunkStreamer::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
}

void ChunkStreamer::update(const glm::vec3& cameraPos) {
    if (!m_chunkManager || !m_generator) {
        return;
    }

    ChunkCoord center = cameraToChunk(cameraPos);
    int viewDistance = std::max(0, m_config.viewDistanceChunks);
    int unloadDistance = std::max(viewDistance, m_config.unloadDistanceChunks);
    int viewRadiusSq = viewDistance * viewDistance;
    int unloadRadiusSq = unloadDistance * unloadDistance;

    std::vector<std::pair<int, ChunkCoord>> desired;
    std::unordered_set<ChunkCoord, ChunkCoordHash> desiredSet;

    for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
        for (int dy = -viewDistance; dy <= viewDistance; ++dy) {
            for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
                ChunkCoord coord{center.x + dx, center.y + dy, center.z + dz};
                int distSq = distanceSquared(center, coord);
                if (distSq > viewRadiusSq) {
                    continue;
                }
                desired.emplace_back(distSq, coord);
                desiredSet.insert(coord);
            }
        }
    }

    std::sort(desired.begin(), desired.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    int generated = 0;
    int maxGenerate = m_config.maxGeneratePerFrame;
    for (const auto& entry : desired) {
        if (maxGenerate > 0 && generated >= maxGenerate) {
            break;
        }

        const ChunkCoord& coord = entry.second;
        m_cache.touch(coord);

        if (m_chunkManager->hasChunk(coord)) {
            continue;
        }

        ChunkBuffer buffer;
        auto start = std::chrono::steady_clock::now();
        m_generator->generate(coord, buffer);

        Chunk& chunk = m_chunkManager->getOrCreateChunk(coord);
        chunk.copyFrom(buffer.blocks);
        if (m_benchmark) {
            auto end = std::chrono::steady_clock::now();
            m_benchmark->addGeneration(
                std::chrono::duration<double>(end - start).count()
            );
        }
        ++generated;
    }

    std::vector<ChunkCoord> toEvict;
    m_chunkManager->forEachChunk([&](ChunkCoord coord, const Chunk&) {
        int distSq = distanceSquared(center, coord);
        if (distSq > unloadRadiusSq) {
            toEvict.push_back(coord);
        }
    });

    for (const ChunkCoord& coord : toEvict) {
        if (m_renderer) {
            m_renderer->removeChunkMesh(coord);
        }
        m_chunkManager->unloadChunk(coord);
        m_cache.erase(coord);
    }

    for (const ChunkCoord& coord : m_cache.evict(desiredSet)) {
        if (m_renderer) {
            m_renderer->removeChunkMesh(coord);
        }
        m_chunkManager->unloadChunk(coord);
    }
}

ChunkCoord ChunkStreamer::cameraToChunk(const glm::vec3& cameraPos) const {
    return worldToChunk(
        static_cast<int>(std::floor(cameraPos.x)),
        static_cast<int>(std::floor(cameraPos.y)),
        static_cast<int>(std::floor(cameraPos.z))
    );
}

} // namespace Rigel::Voxel
