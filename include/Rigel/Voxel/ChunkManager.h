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

#include <unordered_map>
#include <memory>
#include <functional>
#include <vector>

namespace Rigel::Voxel {
class BlockRegistry;

/**
 * @brief Manages multiple chunks in a voxel world.
 *
 * ChunkManager handles:
 * - Chunk storage and lifecycle
 * - World-to-chunk coordinate translation
 * - Dirty chunk tracking for mesh updates
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
 * // Process dirty chunks
 * for (ChunkCoord coord : manager.getDirtyChunks()) {
 *     rebuildMesh(coord, *manager.getChunk(coord));
 * }
 * manager.clearDirtyFlags();
 * @endcode
 */
class ChunkManager {
public:
    ChunkManager() = default;

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

    /// @name Lifecycle
    /// @{

    /**
     * @brief Load chunk from serialized data.
     *
     * @param coord The chunk coordinate
     * @param data Serialized chunk data
     */
    void loadChunk(ChunkCoord coord, std::span<const uint8_t> data);

    /**
     * @brief Unload a chunk.
     *
     * @param coord The chunk coordinate
     * @note Does not save the chunk. Call serialize() first if needed.
     */
    void unloadChunk(ChunkCoord coord);

    /**
     * @brief Unload all chunks.
     */
    void clear();

    /// @}

    /// @name Dirty Tracking
    /// @{

    /**
     * @brief Get list of dirty chunk coordinates.
     * @return Vector of coordinates for chunks needing mesh rebuild
     */
    std::vector<ChunkCoord> getDirtyChunks() const;

    /**
     * @brief Clear dirty flags on all chunks.
     *
     * Call after rebuilding meshes for dirty chunks.
     */
    void clearDirtyFlags();

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
    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> m_chunks;
    const BlockRegistry* m_registry = nullptr;
};

} // namespace Rigel::Voxel
