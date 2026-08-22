#include "Rigel/Voxel/WorldView.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace Rigel::Voxel {

WorldView::WorldView(World& world, WorldResources& resources)
    : m_world(&world)
    , m_resources(&resources)
    , m_streamer(world.chunkManager(),
                 m_meshStore,
                 resources.registry(),
                 &resources.textureAtlas(),
                 world.generator())
{}

void WorldView::initialize(Asset::AssetManager& assets) {
    if (m_initialized) {
        spdlog::warn("WorldView::initialize called multiple times");
        return;
    }

    try {
        m_shader = assets.get<Asset::ShaderAsset>("shaders/voxel");
    } catch (const std::exception& e) {
        spdlog::error("Failed to load voxel shader: {}", e.what());
        throw;
    }
    try {
        m_shadowDepthShader = assets.get<Asset::ShaderAsset>("shaders/voxel_shadow_depth");
        m_shadowTransmitShader = assets.get<Asset::ShaderAsset>("shaders/voxel_shadow_transmit");
    } catch (const std::exception& e) {
        spdlog::warn("Shadow shaders unavailable: {}", e.what());
    }

    m_entityRenderer.initialize(assets);

    m_initialized = true;
}

void WorldView::setGenerator(std::shared_ptr<const WorldGenerator> generator) {
    if (!m_world || !m_resources) {
        return;
    }
    m_streamer.setGenerator(std::move(generator));
}

void WorldView::setChunkLoader(ChunkStreamer::ChunkLoadCallback loader) {
    m_streamer.setChunkLoader(std::move(loader));
}

void WorldView::setChunkPendingCallback(ChunkStreamer::ChunkPendingCallback pending) {
    m_streamer.setChunkPendingCallback(std::move(pending));
}

void WorldView::setChunkLoadDrain(ChunkStreamer::ChunkLoadDrainCallback drain) {
    m_streamer.setChunkLoadDrain(std::move(drain));
}

void WorldView::setChunkLoadCancel(ChunkStreamer::ChunkLoadCancelCallback cancel) {
    m_streamer.setChunkLoadCancel(std::move(cancel));
}

void WorldView::setChunkLoadWorkCallback(ChunkStreamer::ChunkLoadWorkCallback work) {
    m_streamer.setChunkLoadWorkCallback(std::move(work));
}

void WorldView::setChunkEvictionCallback(ChunkStreamer::ChunkEvictionCallback evict) {
    m_streamer.setChunkEvictionCallback(std::move(evict));
}

void WorldView::setStreamConfig(const StreamingConfig& config) {
    m_streamer.setConfig(config);
}

void WorldView::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
    m_streamer.setBenchmark(stats);
}

void WorldView::markSpawnDiscoveryComplete() {
    m_streamer.markSpawnDiscoveryComplete();
}

void WorldView::updateStreaming(const glm::vec3& cameraPos) {
    m_streamer.update(cameraPos);
}

void WorldView::updateMeshes() {
    m_streamer.processCompletions();
}

void WorldView::render(const glm::mat4& view,
                       const glm::mat4& projection,
                       const glm::vec3& cameraPos,
                       float nearPlane,
                       float farPlane,
                       float dt) {
    if (!m_resources) {
        return;
    }

    Entity::EntityRenderContext entityCtx;
    entityCtx.deltaTime = dt;
    entityCtx.frameIndex = ++m_frameCounter;

    struct EntityShadowCaster final : IShadowCaster {
        Entity::EntityRenderer* renderer = nullptr;
        Voxel::World* world = nullptr;
        const Entity::EntityRenderContext* context = nullptr;

        void renderShadowCascade(const ShadowCascadeContext& ctx) override {
            if (!renderer || !world || !context) {
                return;
            }
            renderer->renderShadowCasters(*world, *context, ctx);
        }
    };

    EntityShadowCaster shadowCaster;
    shadowCaster.renderer = &m_entityRenderer;
    shadowCaster.world = m_world;
    shadowCaster.context = &entityCtx;

    WorldRenderContext ctx;
    ctx.meshes = &m_meshStore;
    ctx.atlas = &m_resources->textureAtlas();
    ctx.shader = m_shader;
    ctx.shadowDepthShader = m_shadowDepthShader;
    ctx.shadowTransmitShader = m_shadowTransmitShader;
    ctx.shadowCaster = m_world ? &shadowCaster : nullptr;
    ctx.config = m_renderConfig;
    ctx.view = view;
    ctx.projection = projection;
    ctx.viewProjection = projection * view;
    ctx.cameraPos = cameraPos;
    ctx.nearPlane = nearPlane;
    ctx.farPlane = farPlane;
    ctx.worldTransform = glm::mat4(1.0f);
    m_renderer.render(ctx);

    if (m_world) {
        entityCtx.viewProjection = ctx.viewProjection;
        entityCtx.view = ctx.view;
        entityCtx.cameraPos = cameraPos;
        entityCtx.sunDirection = ctx.config.sunDirection;
        entityCtx.ambientStrength = 0.3f;
        auto shadowState = m_renderer.shadowRenderState();
        entityCtx.shadow.enabled = shadowState.active && ctx.config.shadow.enabled;
        entityCtx.shadow.depthMap = shadowState.depthArray;
        entityCtx.shadow.transmittanceMap = shadowState.transmitArray;
        entityCtx.shadow.cascadeCount = shadowState.cascades;
        entityCtx.shadow.matrices = shadowState.matrices;
        entityCtx.shadow.splits = shadowState.splits;
        entityCtx.shadow.bias = ctx.config.shadow.bias;
        entityCtx.shadow.normalBias = ctx.config.shadow.normalBias;
        entityCtx.shadow.pcfRadius = ctx.config.shadow.pcfRadius;
        entityCtx.shadow.pcfNear = static_cast<float>(ctx.config.shadow.pcfRadiusNear);
        entityCtx.shadow.pcfFar = static_cast<float>(ctx.config.shadow.pcfRadiusFar);
        entityCtx.shadow.strength = ctx.config.shadow.strength;
        entityCtx.shadow.nearPlane = ctx.nearPlane;
        float fadeStart = ctx.config.shadow.maxDistance > 0.0f
            ? std::min(ctx.config.shadow.maxDistance, ctx.farPlane)
            : ctx.farPlane;
        entityCtx.shadow.fadeStart = fadeStart;
        entityCtx.shadow.fadePower = ctx.config.shadow.fadePower;
        m_entityRenderer.render(*m_world, entityCtx);
    }
}

void WorldView::getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out) const {
    m_streamer.getDebugStates(out);
}

int WorldView::viewDistanceChunks() const {
    return m_streamer.viewDistanceChunks();
}

void WorldView::prioritizeChunkMesh(ChunkCoord coord) {
    m_streamer.prioritizeMesh(coord);
}

void WorldView::clear() {
    m_meshStore.clear();
    m_renderer.clearCache();
    m_streamer.reset();
}

void WorldView::releaseRenderResources() {
    m_renderer.releaseResources();
    m_shader = {};
    m_shadowDepthShader = {};
    m_shadowTransmitShader = {};
}

} // namespace Rigel::Voxel
