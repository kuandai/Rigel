#pragma once

#include "ChunkTasks.h"
#include "ChunkCache.h"
#include "ChunkBenchmark.h"
#include "ChunkManager.h"
#include "ChunkMesh.h"
#include "TextureAtlas.h"
#include "WorldMeshStore.h"
#include "WorldGenConfig.h"
#include "WorldGenerator.h"

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/vec3.hpp>
#include <memory>

namespace Rigel::Voxel {

class ChunkStreamer {
public:
    enum class DebugState : uint8_t {
        QueuedGen,
        ReadyData,
        QueuedMesh,
        ReadyMesh
    };

    struct DebugChunkState {
        ChunkCoord coord;
        DebugState state;
    };

    using ChunkLoadCallback = std::function<bool(ChunkCoord)>;
    using ChunkPendingCallback = std::function<bool(ChunkCoord)>;
    using ChunkLoadDrainCallback = std::function<void(size_t)>;
    using ChunkLoadCancelCallback = std::function<void(ChunkCoord)>;

    ChunkStreamer() = default;
    ~ChunkStreamer();

    void setConfig(const WorldGenConfig::StreamConfig& config);
    void bind(ChunkManager* manager,
              WorldMeshStore* meshStore,
              BlockRegistry* registry,
              TextureAtlas* atlas,
              std::shared_ptr<WorldGenerator> generator);
    void setBenchmark(ChunkBenchmarkStats* stats);
    void setChunkLoader(ChunkLoadCallback loader);
    void setChunkPendingCallback(ChunkPendingCallback pending);
    void setChunkLoadDrain(ChunkLoadDrainCallback drain);
    void setChunkLoadCancel(ChunkLoadCancelCallback cancel);

    void update(const glm::vec3& cameraPos);
    void processCompletions();
    void reset();
    void getDebugStates(std::vector<DebugChunkState>& out) const;
    int viewDistanceChunks() const { return m_config.viewDistanceChunks; }

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
        uint32_t worldGenVersion = 0;
        double seconds = 0.0;
        bool cancelled = false;
        std::shared_ptr<std::atomic_bool> cancelToken;
    };

    struct MeshTask {
        ChunkCoord coord;
        uint32_t revision = 0;
        std::array<BlockState, Chunk::VOLUME> blocks{};
        std::array<BlockState, kPaddedVolume> paddedBlocks{};
    };

    struct MeshResult {
        ChunkCoord coord;
        uint32_t revision = 0;
        ChunkMesh mesh;
        double seconds = 0.0;
        bool empty = false;
    };

    enum class MeshRequestKind : uint8_t {
        Missing,
        Dirty
    };

    WorldGenConfig::StreamConfig m_config;
    ChunkManager* m_chunkManager = nullptr;
    WorldMeshStore* m_meshStore = nullptr;
    BlockRegistry* m_registry = nullptr;
    TextureAtlas* m_atlas = nullptr;
    std::shared_ptr<WorldGenerator> m_generator;
    ChunkCache m_cache;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    ChunkLoadCallback m_chunkLoader;
    ChunkPendingCallback m_chunkPending;
    ChunkLoadDrainCallback m_chunkLoadDrain;
    ChunkLoadCancelCallback m_chunkLoadCancel;

    std::unique_ptr<detail::ThreadPool> m_genPool;
    std::unique_ptr<detail::ThreadPool> m_meshPool;
    detail::ConcurrentQueue<GenResult> m_genComplete;
    detail::ConcurrentQueue<MeshResult> m_meshComplete;
    std::unordered_map<ChunkCoord, ChunkState, ChunkCoordHash> m_states;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_loadPending;
    std::unordered_map<ChunkCoord, std::shared_ptr<std::atomic_bool>, ChunkCoordHash> m_genCancel;
    std::unordered_map<ChunkCoord, MeshRequestKind, ChunkCoordHash> m_meshInFlight;
    std::vector<ChunkCoord> m_desired;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_desiredSet;
    size_t m_inFlightGen = 0;
    size_t m_inFlightMesh = 0;
    size_t m_inFlightMeshMissing = 0;
    size_t m_inFlightMeshDirty = 0;
    std::optional<ChunkCoord> m_lastCenter;
    int m_lastViewDistance = -1;
    int m_lastUnloadDistance = -1;
    size_t m_updateCursor = 0;

    void applyGenCompletions(size_t budget);
    void applyMeshCompletions(size_t budget);
    void enqueueGeneration(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord, Chunk& chunk, MeshRequestKind kind);
    void ensureThreadPool();
    bool hasAllNeighborsLoaded(ChunkCoord coord) const;

    ChunkCoord cameraToChunk(const glm::vec3& cameraPos) const;
};

} // namespace Rigel::Voxel
