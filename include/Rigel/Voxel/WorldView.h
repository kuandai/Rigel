#pragma once

#include "ChunkBenchmark.h"
#include "ChunkRenderer.h"
#include "ChunkStreamer.h"
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

namespace Rigel::Voxel {

class WorldView {
public:
    WorldView(World& world, WorldResources& resources);
    ~WorldView() = default;

    WorldView(const WorldView&) = delete;
    WorldView& operator=(const WorldView&) = delete;

    WorldView(WorldView&&) = default;
    WorldView& operator=(WorldView&&) = default;

    void initialize(Asset::AssetManager& assets);

    World& world() { return *m_world; }
    const World& world() const { return *m_world; }

    WorldMeshStore& meshStore() { return m_meshStore; }
    const WorldMeshStore& meshStore() const { return m_meshStore; }

    WorldRenderConfig& renderConfig() { return m_renderConfig; }
    const WorldRenderConfig& renderConfig() const { return m_renderConfig; }

    void setGenerator(std::shared_ptr<const WorldGenerator> generator);
    void setChunkLoader(ChunkStreamer::ChunkLoadCallback loader);
    void setChunkPendingCallback(ChunkStreamer::ChunkPendingCallback pending);
    void setChunkLoadDrain(ChunkStreamer::ChunkLoadDrainCallback drain);
    void setChunkLoadCancel(ChunkStreamer::ChunkLoadCancelCallback cancel);
    void setChunkLoadWorkCallback(ChunkStreamer::ChunkLoadWorkCallback work);
    void setChunkEvictionCallback(ChunkStreamer::ChunkEvictionCallback evict);
    void setStreamConfig(const StreamingConfig& config);
    void setBenchmark(ChunkBenchmarkStats* stats);
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
    void getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out) const;
    int viewDistanceChunks() const;
    void prioritizeChunkMesh(ChunkCoord coord);

    void clear();
    void releaseRenderResources();

private:
    World* m_world = nullptr;
    WorldResources* m_resources = nullptr;
    ChunkRenderer m_renderer;
    WorldMeshStore m_meshStore;
    ChunkStreamer m_streamer;
    WorldRenderConfig m_renderConfig;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    Asset::Handle<Asset::ShaderAsset> m_shadowDepthShader;
    Asset::Handle<Asset::ShaderAsset> m_shadowTransmitShader;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    Entity::EntityRenderer m_entityRenderer;
    uint64_t m_frameCounter = 0;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
