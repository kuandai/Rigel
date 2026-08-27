#pragma once

#include "ChunkBenchmark.h"
#include "ChunkRenderer.h"
#include "ChunkStreamer.h"
#include "ViewDistancePolicy.h"
#include "World.h"
#include "WorldMeshStore.h"
#include "WorldRenderContext.h"
#include "WorldResources.h"

#include <Rigel/Asset/AssetManager.h>
#include <Rigel/Entity/EntityRenderer.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Rigel {
class ApplicationPreferences;
}

namespace Rigel::Voxel {

class WorldView {
public:
    WorldView(World& world, WorldResources& resources);
    ~WorldView() = default;

    WorldView(const WorldView&) = delete;
    WorldView& operator=(const WorldView&) = delete;

    WorldView(WorldView&&) = delete;
    WorldView& operator=(WorldView&&) = delete;

    void initialize(Asset::AssetManager& assets);

    World& world() { return *m_world; }
    const World& world() const { return *m_world; }

    const WorldMeshStore& meshStore() const { return m_meshStore; }

    void setRenderConfig(const WorldRenderConfig& config);
    const WorldRenderConfig& renderConfig() const { return m_renderConfig; }
    const std::shared_ptr<const ViewDistancePolicy>& viewDistancePolicy() const {
        return m_viewDistancePolicy;
    }
    float projectionFarPlaneWorldUnits() const;

    /// Binds streaming to the generator already owned by the world.
    void setGenerator(std::shared_ptr<const WorldGenerator> generator);
    const std::shared_ptr<const WorldGenerator>& generator() const {
        return m_streamer.m_generator;
    }
    void setChunkLoader(ChunkStreamer::ChunkLoadCallback loader);
    void setChunkPendingCallback(ChunkStreamer::ChunkPendingCallback pending);
    void setChunkLoadDrain(ChunkStreamer::ChunkLoadDrainCallback drain);
    void setChunkLoadCancel(ChunkStreamer::ChunkLoadCancelCallback cancel);
    void setChunkLoadDiagnosticsCallback(
        ChunkStreamer::ChunkLoadDiagnosticsCallback diagnostics);
    void setChunkLoadExecutionStateCallback(
        ChunkStreamer::ChunkLoadExecutionStateCallback executionState);
    void setChunkEvictionCallback(ChunkStreamer::ChunkEvictionCallback evict);
    void setStreamConfig(const StreamingConfig& config);
    void setBenchmark(ChunkBenchmarkStats* stats);
    void setVisibilityTracer(std::shared_ptr<ChunkVisibilityTracer> tracer);
    void markSpawnDiscoveryComplete();

    void updateStreaming(const glm::vec3& cameraPos);
    void updateMeshes();
    const ChunkStreamer::WorkMetrics& streamingMetrics() const {
        return m_streamer.workMetrics();
    }
    const StreamingDiagnosticSnapshot& streamingDiagnostics() const {
        return m_streamer.diagnostics();
    }
    void render(const glm::mat4& view,
                const glm::mat4& projection,
                const glm::vec3& cameraPos,
                float nearPlane,
                float farPlane,
                float dt = 0.0f);
    void getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out,
                             ChunkCoord center,
                             int radius) const;
    int viewDistanceChunks() const;
    void prioritizeChunkMesh(ChunkCoord coord);

    /**
     * @brief Reset view-owned streaming and mesh state.
     *
     * Authoritative chunks remain in the world and will be reconsidered for
     * meshing on the next streaming update.
     */
    void clear();
    void releaseRenderResources();

private:
    friend class ::Rigel::ApplicationPreferences;

    void applyViewDistancePolicy(
        std::shared_ptr<const ViewDistancePolicy> policy);

    World* m_world = nullptr;
    WorldResources* m_resources = nullptr;
    ChunkRenderer m_renderer;
    WorldMeshStore m_meshStore;
    ChunkStreamer m_streamer;
    WorldRenderConfig m_renderConfig;
    std::shared_ptr<const ViewDistancePolicy> m_viewDistancePolicy;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    Asset::Handle<Asset::ShaderAsset> m_shadowDepthShader;
    Asset::Handle<Asset::ShaderAsset> m_shadowTransmitShader;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    Entity::EntityRenderer m_entityRenderer;
    uint64_t m_frameCounter = 0;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
