#pragma once

/**
 * @file ChunkRenderer.h
 * @brief Rendering system for voxel chunks.
 *
 * ChunkRenderer consumes world mesh data and manages GPU resources per
 * renderer/context.
 */

#include "Block.h"
#include "ChunkCoord.h"
#include "ChunkMesh.h"
#include "WorldMeshStore.h"
#include "WorldRenderContext.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Asset/Types.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>
#include <GL/glew.h>

namespace Rigel::Voxel {

class WorldView;
namespace detail {
struct WorldViewTestAccess;
}

/**
 * @brief Renders voxel chunks with multi-pass transparency support.
 *
 * Renderer holds GPU resources only. World mesh data is provided through
 * WorldRenderContext each frame.
 */
class ChunkRenderer {
public:
    ChunkRenderer() = default;
    ~ChunkRenderer() = default;

    /// Non-copyable
    ChunkRenderer(const ChunkRenderer&) = delete;
    ChunkRenderer& operator=(const ChunkRenderer&) = delete;

    /// Movable
    ChunkRenderer(ChunkRenderer&&) = default;
    ChunkRenderer& operator=(ChunkRenderer&&) = default;

    /**
     * @brief Render a world context.
     */
    void render(const WorldRenderContext& ctx);

    struct ShadowRenderState {
        bool active = false;
        GLuint depthArray = 0;
        GLuint transmitArray = 0;
        int cascades = 0;
        int mapSize = 0;
        std::array<glm::mat4, ShadowConfig::MaxCascades> matrices{};
        std::array<float, ShadowConfig::MaxCascades> splits{};
    };

    /**
     * @brief Snapshot the latest shadow render state.
     */
    ShadowRenderState shadowRenderState() const;

    /**
     * @brief Check if shadow maps were rendered this frame.
     */
    bool shadowsActive() const { return m_shadowsActive; }

    /**
     * @brief Clear all GPU-resident meshes.
     */
    void clearCache();

    /**
     * @brief Release GPU resources owned by the renderer.
     */
    void releaseResources();

    /**
     * @brief Get number of cached GPU meshes.
     */
    size_t cachedMeshCount() const { return m_meshes.size(); }

    bool hasDrawnMesh(uint32_t storeId,
                      ChunkCoord coord,
                      MeshRevision revision) const;

private:
    friend class WorldView;
    friend struct detail::WorldViewTestAccess;

    static constexpr int kMaxShadowCascades = ShadowConfig::MaxCascades;

    struct GpuMesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        size_t indexCount = 0;
        std::array<ChunkMesh::LayerRange, RenderLayerCount> layers{};

        GpuMesh() = default;
        ~GpuMesh();

        GpuMesh(const GpuMesh&) = delete;
        GpuMesh& operator=(const GpuMesh&) = delete;

        GpuMesh(GpuMesh&& other) noexcept;
        GpuMesh& operator=(GpuMesh&& other) noexcept;

        bool isValid() const { return vao != 0 && indexCount > 0; }

        void release();
    };

    struct GpuMeshEntry {
        ChunkCoord coord{};
        MeshRevision revision{};
        GpuMesh mesh;
        bool drawn = false;
    };

    std::unordered_map<MeshId, GpuMeshEntry, MeshIdHash> m_meshes;
    std::unordered_map<uint32_t, uint64_t> m_storeVersions;

    Asset::Handle<Asset::ShaderAsset> m_shader;
    Asset::Handle<Asset::ShaderAsset> m_shadowDepthShader;
    Asset::Handle<Asset::ShaderAsset> m_shadowTransmitShader;
    const TextureAtlas* m_atlas = nullptr;

    // Cached uniform locations
    GLint m_locViewProjection = -1;
    GLint m_locChunkOffset = -1;
    GLint m_locTextureAtlas = -1;
    GLint m_locSunDirection = -1;
    GLint m_locAlphaMultiplier = -1;
    GLint m_locAlphaCutoff = -1;
    GLint m_locView = -1;
    GLint m_locRenderLayer = -1;
    GLint m_locShadowEnabled = -1;
    GLint m_locShadowMap = -1;
    GLint m_locShadowTransmittanceMap = -1;
    GLint m_locShadowMatrices = -1;
    GLint m_locShadowSplits = -1;
    GLint m_locShadowCascadeCount = -1;
    GLint m_locShadowBias = -1;
    GLint m_locShadowNormalBias = -1;
    GLint m_locShadowStrength = -1;
    GLint m_locShadowNear = -1;
    GLint m_locShadowFadeStart = -1;
    GLint m_locShadowPcfNear = -1;
    GLint m_locShadowPcfFar = -1;
    GLint m_locShadowFadePower = -1;

    struct ShadowUniforms {
        GLint lightViewProjection = -1;
        GLint chunkOffset = -1;
        GLint textureAtlas = -1;
        GLint alphaCutoff = -1;
        GLint tintAtlas = -1;
        GLint transparentScale = -1;
    };

    ShadowUniforms m_shadowDepthUniforms;
    ShadowUniforms m_shadowTransmitUniforms;

    struct RenderEntry {
        ChunkCoord coord{};
        MeshId meshId{};
        float distanceSq = 0.0f;
        float viewDepth = 0.0f;
        std::optional<ChunkVisibilityTraceLink> visibilityTrace;
    };

    struct ShadowState {
        GLuint depthArray = 0;
        GLuint transmitArray = 0;
        GLuint fbo = 0;
        int cascades = 0;
        int mapSize = 0;
        std::array<glm::mat4, kMaxShadowCascades> matrices{};
        std::array<float, kMaxShadowCascades> splits{};
    };

    class PreparedShadowResources final {
    public:
        PreparedShadowResources(PreparedShadowResources&& other) noexcept;
        PreparedShadowResources& operator=(
            PreparedShadowResources&& other) noexcept;
        ~PreparedShadowResources();

        PreparedShadowResources(const PreparedShadowResources&) = delete;
        PreparedShadowResources& operator=(
            const PreparedShadowResources&) = delete;

    private:
        PreparedShadowResources() = default;

        ShadowState m_state;

        friend class ChunkRenderer;
    };

    ShadowState m_shadowState;
    bool m_shadowsActive = false;

    void uploadMesh(GpuMesh& gpu, const ChunkMesh& mesh) const;
    void pruneCache(const WorldMeshStore& store);
    void cacheUniformLocations();
    void cacheShadowUniforms();
    void renderPass(RenderLayer layer,
                    const std::vector<RenderEntry>& entries,
                    const WorldRenderContext& ctx);
    void setupLayerState(RenderLayer layer) const;
    static void releaseShadowResources(ShadowState& state) noexcept;
    void releaseShadowResources();
    PreparedShadowResources prepareShadowResources(
        bool enabled,
        const ShadowConfig& config) const;
    PreparedShadowResources installShadowResources(
        PreparedShadowResources prepared) noexcept;
    bool shadowResourcesInstalled() const;
    bool renderShadows(const WorldRenderContext& ctx,
                       const std::vector<RenderEntry>& entries);
    void renderShadowLayer(const std::vector<RenderEntry>& entries,
                           RenderLayer layer,
                           const WorldRenderContext& ctx,
                           const ShadowUniforms& uniforms) const;
    int countShadowDraws(const std::vector<RenderEntry>& entries, RenderLayer layer) const;
};

} // namespace Rigel::Voxel
