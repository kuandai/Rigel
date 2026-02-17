#pragma once

#include "ChunkBenchmark.h"
#include "ChunkRenderer.h"
#include "ChunkStreamer.h"
#include "Lod/SvoLodManager.h"
#include "MeshBuilder.h"
#include "World.h"
#include "WorldGenConfig.h"
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
#include <span>
#include <unordered_set>
#include <vector>

namespace Rigel::Voxel {

struct WorldReplicationState {
    std::unordered_set<ChunkCoord, ChunkCoordHash> knownChunks;
};

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

    void setRenderConfig(const WorldRenderConfig& config);
    const WorldRenderConfig& renderConfig() const { return m_renderConfig; }
    const SvoLodConfig& svoConfig() const { return m_svoLod.config(); }
    const SvoLodTelemetry& svoTelemetry() const { return m_svoLod.telemetry(); }

    WorldReplicationState& replicationState() { return m_replication; }
    const WorldReplicationState& replicationState() const { return m_replication; }

    void setGenerator(std::shared_ptr<WorldGenerator> generator);
    void setChunkLoader(ChunkStreamer::ChunkLoadCallback loader);
    void setChunkPendingCallback(ChunkStreamer::ChunkPendingCallback pending);
    void setChunkLoadDrain(ChunkStreamer::ChunkLoadDrainCallback drain);
    void setChunkLoadCancel(ChunkStreamer::ChunkLoadCancelCallback cancel);
    void setStreamConfig(const WorldGenConfig::StreamConfig& config);
    void setBenchmark(ChunkBenchmarkStats* stats);

    void updateStreaming(const glm::vec3& cameraPos);
    void updateMeshes();
    void render(const glm::mat4& view,
                const glm::mat4& projection,
                const glm::vec3& cameraPos,
                float nearPlane,
                float farPlane,
                float dt = 0.0f);
    void getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out) const;
    int viewDistanceChunks() const;
    void rebuildChunkMesh(ChunkCoord coord);

    void applyChunkDelta(ChunkCoord coord, std::span<const uint8_t> payload);

    void clear();
    void releaseRenderResources();

private:
    World* m_world = nullptr;
    WorldResources* m_resources = nullptr;
    MeshBuilder m_meshBuilder;
    ChunkRenderer m_renderer;
    WorldMeshStore m_meshStore;
    ChunkStreamer m_streamer;
    SvoLodManager m_svoLod;
    WorldRenderConfig m_renderConfig;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    Asset::Handle<Asset::ShaderAsset> m_shadowDepthShader;
    Asset::Handle<Asset::ShaderAsset> m_shadowTransmitShader;
    ChunkBenchmarkStats* m_benchmark = nullptr;
    WorldReplicationState m_replication;
    Entity::EntityRenderer m_entityRenderer;
    uint64_t m_frameCounter = 0;
    bool m_initialized = false;
};

} // namespace Rigel::Voxel
