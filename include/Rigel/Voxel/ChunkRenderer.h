#pragma once

/**
 * @file ChunkRenderer.h
 * @brief Rendering system for voxel chunks.
 *
 * ChunkRenderer handles multi-pass rendering of chunk meshes with proper
 * layer ordering for transparency.
 */

#include "Block.h"
#include "ChunkMesh.h"
#include "ChunkCoord.h"
#include "TextureAtlas.h"

#include <Rigel/Asset/Types.h>
#include <Rigel/Asset/Handle.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <unordered_map>

namespace Rigel::Voxel {

/**
 * @brief Renders voxel chunks with multi-pass transparency support.
 *
 * Manages a collection of chunk meshes and renders them in the correct
 * order for transparency. Supports:
 * - Opaque pass (depth write, no blend)
 * - Cutout pass (alpha test)
 * - Transparent pass (alpha blend, sorted)
 * - Emissive pass (additive blend)
 *
 * @section usage Usage
 *
 * @code
 * ChunkRenderer renderer;
 * renderer.setShader(myShader);
 * renderer.setTextureAtlas(&atlas);
 *
 * // Add meshes
 * renderer.setChunkMesh(chunkCoord, std::move(mesh));
 *
 * // Each frame
 * renderer.render(viewProjection, cameraPosition);
 * @endcode
 */
class ChunkRenderer {
public:
    /**
     * @brief Renderer configuration.
     */
    struct Config {
        float renderDistance = 256.0f;  ///< Max distance to render chunks
        glm::vec3 sunDirection = glm::vec3(0.5f, 1.0f, 0.3f);  ///< Sun direction for lighting
        float transparentAlpha = 0.5f;  ///< Alpha multiplier for transparent layer
    };

    /**
     * @brief Construct renderer with configuration.
     */
    ChunkRenderer();
    explicit ChunkRenderer(const Config& config);

    ~ChunkRenderer() = default;

    /// Non-copyable
    ChunkRenderer(const ChunkRenderer&) = delete;
    ChunkRenderer& operator=(const ChunkRenderer&) = delete;

    /// Movable
    ChunkRenderer(ChunkRenderer&&) = default;
    ChunkRenderer& operator=(ChunkRenderer&&) = default;

    /**
     * @brief Set the shader program for rendering.
     *
     * @param shader Handle to shader asset
     */
    void setShader(Asset::Handle<Asset::ShaderAsset> shader);

    /**
     * @brief Set the texture atlas.
     *
     * @param atlas Pointer to texture atlas (must remain valid)
     */
    void setTextureAtlas(TextureAtlas* atlas);

    /**
     * @brief Add or update mesh for a chunk.
     *
     * @param coord Chunk coordinate
     * @param mesh The mesh to store (moved)
     */
    void setChunkMesh(ChunkCoord coord, ChunkMesh mesh);

    /**
     * @brief Remove mesh for a chunk.
     *
     * @param coord Chunk coordinate
     */
    void removeChunkMesh(ChunkCoord coord);

    /**
     * @brief Check if a chunk has a mesh.
     */
    bool hasChunkMesh(ChunkCoord coord) const;

    /**
     * @brief Get number of stored meshes.
     */
    size_t meshCount() const { return m_meshes.size(); }

    /**
     * @brief Clear all stored meshes.
     */
    void clear();

    /**
     * @brief Render all visible chunks.
     *
     * @param viewProjection Combined view-projection matrix
     * @param cameraPos Camera world position (for distance culling)
     */
    void render(const glm::mat4& viewProjection, const glm::vec3& cameraPos);

    /**
     * @brief Set sun direction for lighting.
     */
    void setSunDirection(const glm::vec3& dir);

    /**
     * @brief Get current configuration.
     */
    const Config& config() const { return m_config; }

    /**
     * @brief Modify configuration.
     */
    Config& config() { return m_config; }

private:
    Config m_config;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    TextureAtlas* m_atlas = nullptr;

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> m_meshes;

    // Cached uniform locations
    GLint m_locViewProjection = -1;
    GLint m_locChunkOffset = -1;
    GLint m_locTextureAtlas = -1;
    GLint m_locSunDirection = -1;
    GLint m_locAlphaMultiplier = -1;
    GLint m_locAlphaCutoff = -1;

    void cacheUniformLocations();
    void renderPass(RenderLayer layer, const glm::mat4& viewProjection, const glm::vec3& cameraPos);
    void setupLayerState(RenderLayer layer);
};

} // namespace Rigel::Voxel
