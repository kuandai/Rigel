#pragma once

/**
 * @file World.h
 * @brief Top-level facade for the voxel system.
 *
 * World coordinates all voxel components: block registry, chunk management,
 * mesh generation, texture atlas, and rendering.
 */

#include "Block.h"
#include "BlockRegistry.h"
#include "ChunkManager.h"
#include "MeshBuilder.h"
#include "ChunkRenderer.h"
#include "TextureAtlas.h"
#include "WorldMeshStore.h"
#include "WorldRenderContext.h"
#include "ChunkBenchmark.h"
#include "ChunkStreamer.h"
#include "WorldGenerator.h"

#include <Rigel/Asset/AssetManager.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <vector>

namespace Rigel::Voxel {

/**
 * @brief Top-level facade for the voxel system.
 *
 * World provides a high-level interface for working with voxels:
 * - Block type registration
 * - Block placement and queries
 * - Automatic mesh updates
 * - Rendering
 *
 * @section usage Usage
 *
 * @code
 * // Create world
 * Voxel::World world;
 *
 * // Initialize with asset manager
 * world.initialize(assets);
 *
 * // Register block types
 * BlockType stone;
 * stone.identifier = "rigel:stone";
 * stone.textures = FaceTextures::uniform("textures/stone.png");
 * world.blockRegistry().registerBlock(stone.identifier, stone);
 *
 * // Place blocks
 * world.setBlock(0, 0, 0, BlockState{stoneId});
 *
 * // Each frame
 * world.updateMeshes();
 * world.render(viewProjection, cameraPos);
 * @endcode
 */
class World {
public:
    World();
    ~World() = default;

    /// Non-copyable
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /// Movable
    World(World&&) = default;
    World& operator=(World&&) = default;

    /**
     * @brief Initialize with asset manager.
     *
     * Loads the voxel shader from the asset manifest.
     *
     * @param assets Asset manager to load from
     */
    void initialize(Asset::AssetManager& assets);

    /// @name Component Access
    /// @{

    /// Get the block registry for type registration
    BlockRegistry& blockRegistry() { return m_blockRegistry; }
    const BlockRegistry& blockRegistry() const { return m_blockRegistry; }

    /// Get the chunk manager for direct chunk access
    ChunkManager& chunkManager() { return m_chunkManager; }
    const ChunkManager& chunkManager() const { return m_chunkManager; }

    /// Get the texture atlas
    TextureAtlas& textureAtlas() { return m_textureAtlas; }
    const TextureAtlas& textureAtlas() const { return m_textureAtlas; }

    /// Get the renderer
    ChunkRenderer& renderer() { return m_renderer; }
    const ChunkRenderer& renderer() const { return m_renderer; }

    /// Access the world mesh store
    WorldMeshStore& meshStore() { return m_meshStore; }
    const WorldMeshStore& meshStore() const { return m_meshStore; }

    /// Access render configuration owned by the world
    WorldRenderConfig& renderConfig() { return m_renderConfig; }
    const WorldRenderConfig& renderConfig() const { return m_renderConfig; }

    /// @}

    /// @name Block Access (World Coordinates)
    /// @{

    /**
     * @brief Set block at world coordinates.
     *
     * Creates the containing chunk if needed.
     *
     * @param wx World X
     * @param wy World Y
     * @param wz World Z
     * @param state Block state to set
     */
    void setBlock(int wx, int wy, int wz, BlockState state);

    /**
     * @brief Get block at world coordinates.
     *
     * Returns air if chunk is not loaded.
     */
    BlockState getBlock(int wx, int wy, int wz) const;

    /// @}

    /// @name Update and Render
    /// @{

    /**
     * @brief Rebuild meshes for dirty chunks.
     *
     * Call once per frame before render().
     */
    void updateMeshes();

    /**
     * @brief Stream/generate chunks around the camera.
     *
     * @param cameraPos Camera world position
     */
    void updateStreaming(const glm::vec3& cameraPos);
    void setBenchmark(ChunkBenchmarkStats* stats);

    /**
     * @brief Render the world.
     *
     * @param viewProjection View-projection matrix
     * @param cameraPos Camera position for distance culling
     */
    void render(const glm::mat4& viewProjection, const glm::vec3& cameraPos);

    /**
     * @brief Populate debug chunk states for visualization.
     */
    void getChunkDebugStates(std::vector<ChunkStreamer::DebugChunkState>& out) const;

    /**
     * @brief Get the current streaming view distance in chunks.
     */
    int viewDistanceChunks() const;

    /**
     * @brief Rebuild mesh for a specific chunk immediately.
     */
    void rebuildChunkMesh(ChunkCoord coord);

    /// @}

    /// @name Lifecycle
    /// @{

    /**
     * @brief Unload all chunks and clear meshes.
     */
    void clear();

    /**
     * @brief Release GPU resources owned by the world.
     *
     * Call before destroying the OpenGL context.
     */
    void releaseRenderResources();

    /// @}

    /// @name Generation
    /// @{

    void setGenerator(std::shared_ptr<WorldGenerator> generator);
    void setStreamConfig(const WorldGenConfig::StreamConfig& config);

    /// @}

private:
    BlockRegistry m_blockRegistry;
    ChunkManager m_chunkManager;
    MeshBuilder m_meshBuilder;
    ChunkRenderer m_renderer;
    WorldMeshStore m_meshStore;
    TextureAtlas m_textureAtlas;
    ChunkStreamer m_streamer;
    std::shared_ptr<WorldGenerator> m_generator;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    WorldRenderConfig m_renderConfig;
    ChunkBenchmarkStats* m_benchmark = nullptr;

    bool m_initialized = false;

};

} // namespace Rigel::Voxel
