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
#include <unordered_map>
#include <vector>
#include <GL/glew.h>

namespace Rigel::Voxel {

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

    /**
     * @brief Clear all GPU-resident meshes.
     */
    void clearCache();

    /**
     * @brief Get number of cached GPU meshes.
     */
    size_t cachedMeshCount() const { return m_meshes.size(); }

private:
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
    };

    std::unordered_map<MeshId, GpuMeshEntry, MeshIdHash> m_meshes;
    std::unordered_map<uint32_t, uint64_t> m_storeVersions;

    Asset::Handle<Asset::ShaderAsset> m_shader;
    const TextureAtlas* m_atlas = nullptr;

    // Cached uniform locations
    GLint m_locViewProjection = -1;
    GLint m_locChunkOffset = -1;
    GLint m_locTextureAtlas = -1;
    GLint m_locSunDirection = -1;
    GLint m_locAlphaMultiplier = -1;
    GLint m_locAlphaCutoff = -1;

    struct RenderEntry {
        ChunkCoord coord{};
        MeshId meshId{};
        float distanceSq = 0.0f;
    };

    void uploadMesh(GpuMesh& gpu, const ChunkMesh& mesh) const;
    void pruneCache(const WorldMeshStore& store);
    void cacheUniformLocations();
    void renderPass(RenderLayer layer,
                    const std::vector<RenderEntry>& entries,
                    const WorldRenderContext& ctx);
    void setupLayerState(RenderLayer layer) const;
};

} // namespace Rigel::Voxel
