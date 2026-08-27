#include "Rigel/Voxel/WorldView.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <stdexcept>

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
    const auto loadOptionalShadow = [&assets](const char* id) {
        if (!assets.exists(id)) {
            spdlog::warn("Optional startup resource '{}' is absent", id);
            return Asset::Handle<Asset::ShaderAsset>{};
        }
        try {
            return assets.get<Asset::ShaderAsset>(id);
        } catch (const std::exception& error) {
            spdlog::warn(
                "Optional startup resource '{}' failed to load: {}",
                id,
                error.what());
            return Asset::Handle<Asset::ShaderAsset>{};
        }
    };
    auto shadowDepth = loadOptionalShadow("shaders/voxel_shadow_depth");
    auto shadowTransmit = loadOptionalShadow("shaders/voxel_shadow_transmit");
    if (shadowDepth) {
        m_shadowDepthShader = std::move(shadowDepth);
    }
    if (shadowTransmit) {
        m_shadowTransmitShader = std::move(shadowTransmit);
    }

    m_entityRenderer.initialize(assets);

    m_initialized = true;
}

void WorldView::setGenerator(std::shared_ptr<const WorldGenerator> generator) {
    if (!m_world || !m_resources) {
        return;
    }
    const auto& worldGenerator = m_world->generator();
    if (!worldGenerator) {
        throw std::invalid_argument(
            "WorldView requires a world-owned generator");
    }
    if (!generator || !worldGenerator->matchesGenerationInputs(
            generator->definition(),
            generator->seed(),
            generator->semanticsVersion())) {
        throw std::invalid_argument(
            "WorldView generator must match the world-owned generator");
    }
    generator = worldGenerator;
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

void WorldView::setChunkLoadDiagnosticsCallback(
    ChunkStreamer::ChunkLoadDiagnosticsCallback diagnostics) {
    m_streamer.setChunkLoadDiagnosticsCallback(std::move(diagnostics));
}

void WorldView::setChunkLoadExecutionStateCallback(
    ChunkStreamer::ChunkLoadExecutionStateCallback executionState) {
    m_streamer.setChunkLoadExecutionStateCallback(std::move(executionState));
}

void WorldView::setChunkEvictionCallback(ChunkStreamer::ChunkEvictionCallback evict) {
    m_streamer.setChunkEvictionCallback(std::move(evict));
}

void WorldView::setStreamConfig(const StreamingConfig& config) {
    StreamingConfig effective = config;
    if (m_viewDistancePolicy) {
        effective.viewDistanceChunks =
            m_viewDistancePolicy->desiredRadiusChunks();
        effective.unloadDistanceChunks =
            m_viewDistancePolicy->unloadRadiusChunks();
    }
    m_streamer.setConfig(effective);
}

void WorldView::setRenderProfileForDiagnostics(
    const RenderProfile& profile) {
    m_renderProfile = profile;
}

WorldView::PreparedShadowChange WorldView::prepareShadowPreference(
    bool enabled) const {
    if (enabled && !m_initialized) {
        throw std::runtime_error(
            "shadows require an initialized world renderer");
    }
    if (enabled && !m_shadowDepthShader) {
        throw std::runtime_error(
            "the voxel shadow depth shader is unavailable");
    }
    return PreparedShadowChange{
        m_renderer.prepareShadowResources(enabled, m_renderProfile.shadow)};
}

WorldView::PreparedShadowChange WorldView::installShadowPreference(
    PreparedShadowChange prepared) noexcept {
    return PreparedShadowChange{m_renderer.installShadowResources(
        std::move(prepared.m_resources))};
}

bool WorldView::shadowPreferenceEnabled() const {
    return m_renderer.shadowResourcesInstalled();
}

WorldView::ViewDistancePolicyState WorldView::applyViewDistancePolicy(
    std::shared_ptr<const ViewDistancePolicy> policy) {
    if (!policy) {
        throw std::invalid_argument(
            "WorldView requires a complete View Distance policy");
    }
    ViewDistancePolicyState previous{
        .policy = m_viewDistancePolicy,
        .streaming = m_streamer.applyViewDistancePolicy(policy)
    };
    m_viewDistancePolicy = std::move(policy);
    return previous;
}

void WorldView::restoreViewDistancePolicy(
    ViewDistancePolicyState state) noexcept {
    m_streamer.restoreViewDistancePolicy(std::move(state.streaming));
    m_viewDistancePolicy = std::move(state.policy);
}

float WorldView::projectionFarPlaneWorldUnits() const {
    if (m_viewDistancePolicy) {
        return m_viewDistancePolicy->projectionFarPlaneWorldUnits();
    }
    return 500.0f;
}

float WorldView::renderDistanceWorldUnits() const {
    return m_viewDistancePolicy
        ? m_viewDistancePolicy->renderDistanceWorldUnits()
        : 256.0f;
}

float WorldView::shadowDistanceWorldUnits() const {
    const float profileLimit =
        m_renderProfile.shadow.maximumDistanceWorldUnits;
    if (!m_viewDistancePolicy) {
        return profileLimit;
    }
    const float viewDistanceCeiling =
        m_viewDistancePolicy->shadowDistanceCeilingWorldUnits();
    return profileLimit > 0.0f
        ? std::min(profileLimit, viewDistanceCeiling)
        : viewDistanceCeiling;
}

void WorldView::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
    m_streamer.setBenchmark(stats);
}

void WorldView::setVisibilityTracer(
    std::shared_ptr<ChunkVisibilityTracer> tracer) {
    m_streamer.setVisibilityTracer(std::move(tracer));
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
    ctx.profile = m_renderProfile;
    ctx.renderDistanceWorldUnits = renderDistanceWorldUnits();
    ctx.shadowDistanceWorldUnits = shadowDistanceWorldUnits();
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
        entityCtx.sunDirection = ctx.profile.sunDirection;
        entityCtx.ambientStrength = 0.3f;
        auto shadowState = m_renderer.shadowRenderState();
        entityCtx.shadow.enabled = shadowState.active;
        entityCtx.shadow.depthMap = shadowState.depthArray;
        entityCtx.shadow.transmittanceMap = shadowState.transmitArray;
        entityCtx.shadow.cascadeCount = shadowState.cascades;
        entityCtx.shadow.matrices = shadowState.matrices;
        entityCtx.shadow.splits = shadowState.splits;
        entityCtx.shadow.bias = ctx.profile.shadow.bias;
        entityCtx.shadow.normalBias = ctx.profile.shadow.normalBias;
        entityCtx.shadow.pcfNear = static_cast<float>(
            ctx.profile.shadow.pcfRadiusNear);
        entityCtx.shadow.pcfFar = static_cast<float>(
            ctx.profile.shadow.pcfRadiusFar);
        entityCtx.shadow.strength = ctx.profile.shadow.strength;
        entityCtx.shadow.nearPlane = ctx.nearPlane;
        float fadeStart = ctx.shadowDistanceWorldUnits > 0.0f
            ? std::min(ctx.shadowDistanceWorldUnits, ctx.farPlane)
            : ctx.farPlane;
        entityCtx.shadow.fadeStart = fadeStart;
        entityCtx.shadow.fadePower = ctx.profile.shadow.fadePower;
        m_entityRenderer.render(*m_world, entityCtx);
    }
}

void WorldView::getChunkDebugStates(
    std::vector<ChunkStreamer::DebugChunkState>& out,
    ChunkCoord center,
    int radius) const {
    m_streamer.getDebugStates(out, center, radius);
    for (auto& state : out) {
        if (state.installedGeometry !=
                ChunkStreamer::DebugInstalledGeometry::Nonempty ||
            state.installedGeometryRevision == 0) {
            continue;
        }
        state.drawEvidence = m_renderer.hasDrawnMesh(
                m_meshStore.storeId(),
                state.coord,
                MeshRevision{state.installedGeometryRevision})
            ? ChunkStreamer::DebugDrawEvidence::Drawn
            : ChunkStreamer::DebugDrawEvidence::NotDrawn;
    }
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
