#pragma once

/**
 * @file ChunkManager.h
 * @brief Multi-chunk management for the voxel system.
 *
 * ChunkManager provides a unified interface for accessing blocks across
 * multiple chunks using world coordinates.
 */

#include "Chunk.h"
#include "ChunkCoord.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <vector>

namespace Rigel::Persistence {
class AsyncChunkLoader;
}

namespace Rigel::Voxel {
class BlockRegistry;
class ChunkStreamer;

/**
 * @brief Manages multiple chunks in a voxel world.
 *
 * ChunkManager handles:
 * - Chunk storage and lifecycle
 * - World-to-chunk coordinate translation
 * - Mesh invalidation notifications
 *
 * @section usage Usage
 *
 * @code
 * ChunkManager manager;
 *
 * // Access chunks
 * Chunk& chunk = manager.getOrCreateChunk({0, 0, 0});
 *
 * // Access blocks using world coordinates
 * manager.setBlock(10, 5, 20, someState);
 * BlockState state = manager.getBlock(10, 5, 20);
 *
 * @endcode
 */
class ChunkManager {
public:
    ChunkManager() = default;
    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;
    ChunkManager(ChunkManager&&) = delete;
    ChunkManager& operator=(ChunkManager&&) = delete;

    /// @name Chunk Access
    /// @{

    /**
     * @brief Get chunk at coordinate (may be null).
     * @param coord The chunk coordinate
     * @return Pointer to chunk or nullptr if not loaded
     */
    Chunk* getChunk(ChunkCoord coord);

    /**
     * @brief Get chunk at coordinate (const, may be null).
     * @param coord The chunk coordinate
     * @return Const pointer to chunk or nullptr if not loaded
     */
    const Chunk* getChunk(ChunkCoord coord) const;

    /**
     * @brief Get or create chunk at coordinate.
     *
     * Creates an empty chunk if one doesn't exist.
     *
     * @param coord The chunk coordinate
     * @return Reference to the chunk
     */
    Chunk& getOrCreateChunk(ChunkCoord coord);

    /**
     * @brief Check if a chunk is loaded.
     * @param coord The chunk coordinate
     * @return True if chunk exists
     */
    bool hasChunk(ChunkCoord coord) const;

    /// @}

    /// @name Block Access (World Coordinates)
    /// @{

    /**
     * @brief Get block at world coordinates.
     *
     * Returns air if the containing chunk is not loaded.
     *
     * @param wx World X coordinate
     * @param wy World Y coordinate
     * @param wz World Z coordinate
     * @return The block state (air if chunk not loaded)
     */
    BlockState getBlock(int wx, int wy, int wz) const;

    /**
     * @brief Set block at world coordinates.
     *
     * Creates the containing chunk if it doesn't exist.
     * Marks the chunk as dirty.
     *
     * @param wx World X coordinate
     * @param wy World Y coordinate
     * @param wz World Z coordinate
     * @param state The new block state
     */
    void setBlock(int wx, int wy, int wz, BlockState state);

    /// @}

    /// @name Iteration
    /// @{

    /**
     * @brief Iterate over all loaded chunks.
     * @param fn Callback receiving chunk coordinate and reference
     */
    void forEachChunk(const std::function<void(ChunkCoord, Chunk&)>& fn);

    /**
     * @brief Iterate over all loaded chunks (const).
     * @param fn Callback receiving chunk coordinate and const reference
     */
    void forEachChunk(const std::function<void(ChunkCoord, const Chunk&)>& fn) const;

    /// @}

    /// @name Statistics
    /// @{

    /// Get number of loaded chunks
    size_t loadedChunkCount() const { return m_chunks.size(); }

    /// @}

    /**
     * @brief Provide block registry for opacity tracking.
     */
    void setRegistry(const BlockRegistry* registry) { m_registry = registry; }

private:
    friend class Chunk;
    friend class ChunkStreamer;
    friend class Rigel::Persistence::AsyncChunkLoader;

    void unloadChunk(ChunkCoord coord);
    void invalidateFaceNeighbors(ChunkCoord coord);
    void notifyMeshChange(ChunkCoord coord);
    std::vector<ChunkCoord> consumeDirtyMeshNotifications();

    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> m_chunks;
    std::deque<ChunkCoord> m_dirtyMeshQueue;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_dirtyMeshQueued;
    const BlockRegistry* m_registry = nullptr;
};

} // namespace Rigel::Voxel
