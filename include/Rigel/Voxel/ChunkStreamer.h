#pragma once

#include "ChunkTasks.h"
#include "ChunkCache.h"
#include "ChunkBenchmark.h"
#include "ChunkManager.h"
#include "ChunkRenderer.h"
#include "ChunkMesh.h"
#include "TextureAtlas.h"
#include "WorldGenConfig.h"
#include "WorldGenerator.h"

#include <array>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/vec3.hpp>
#include <memory>

namespace Rigel::Voxel {

class ChunkStreamer {
public:
    ChunkStreamer() = default;
    ~ChunkStreamer() = default;

    void setConfig(const WorldGenConfig::StreamConfig& config);
    void bind(ChunkManager* manager,
              ChunkRenderer* renderer,
              BlockRegistry* registry,
              TextureAtlas* atlas,
              std::shared_ptr<WorldGenerator> generator);
    void setBenchmark(ChunkBenchmarkStats* stats);

    void update(const glm::vec3& cameraPos);
    void processCompletions();
    void reset();

private:
    static constexpr int kPaddedSize = Chunk::SIZE + 2;
    static constexpr int kPaddedVolume = kPaddedSize * kPaddedSize * kPaddedSize;

    enum class ChunkState : uint8_t {
        Missing,
        QueuedGen,
        ReadyData,
        QueuedMesh,
        ReadyMesh
    };

    struct GenResult {
        ChunkCoord coord;
        std::array<BlockState, Chunk::VOLUME> blocks{};
        double seconds = 0.0;
        bool cancelled = false;
        std::shared_ptr<std::atomic_bool> cancelToken;
    };

    struct MeshTask {
        ChunkCoord coord;
        std::array<BlockState, Chunk::VOLUME> blocks{};
        std::array<BlockState, kPaddedVolume> paddedBlocks{};
    };

    struct MeshResult {
        ChunkCoord coord;
        ChunkMesh mesh;
        double seconds = 0.0;
        bool empty = false;
    };

    WorldGenConfig::StreamConfig m_config;
    ChunkManager* m_chunkManager = nullptr;
    ChunkRenderer* m_renderer = nullptr;
    BlockRegistry* m_registry = nullptr;
    TextureAtlas* m_atlas = nullptr;
    std::shared_ptr<WorldGenerator> m_generator;
    ChunkCache m_cache;
    ChunkBenchmarkStats* m_benchmark = nullptr;

    std::unique_ptr<detail::ThreadPool> m_pool;
    detail::ConcurrentQueue<GenResult> m_genComplete;
    detail::ConcurrentQueue<MeshResult> m_meshComplete;
    std::unordered_map<ChunkCoord, ChunkState, ChunkCoordHash> m_states;
    std::unordered_map<ChunkCoord, std::shared_ptr<std::atomic_bool>, ChunkCoordHash> m_genCancel;
    std::vector<ChunkCoord> m_desired;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_desiredSet;
    size_t m_inFlightGen = 0;
    size_t m_inFlightMesh = 0;
    std::optional<ChunkCoord> m_lastCenter;
    int m_lastViewDistance = -1;
    int m_lastUnloadDistance = -1;

    void applyGenCompletions(size_t budget);
    void applyMeshCompletions(size_t budget);
    void enqueueGeneration(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord, Chunk& chunk);
    void ensureThreadPool();

    ChunkCoord cameraToChunk(const glm::vec3& cameraPos) const;
};

} // namespace Rigel::Voxel
