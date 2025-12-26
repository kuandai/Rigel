#pragma once

/**
 * @file Chunk.h
 * @brief Chunk storage for the voxel system.
 *
 * A Chunk is a fixed-size cubic region of blocks. The default size is
 * 32x32x32 = 32,768 blocks per chunk.
 */

#include "Block.h"
#include "ChunkCoord.h"

#include <array>
#include <span>
#include <vector>
#include <cstdint>

namespace Rigel::Voxel {

/**
 * @brief Fixed-size cubic region of blocks.
 *
 * Chunks store blocks in a flat array indexed by [x + y*SIZE + z*SIZE*SIZE].
 * Dirty tracking indicates when the chunk's mesh needs to be rebuilt.
 *
 * @section memory Memory Usage
 *
 * With 4-byte BlockState and SIZE=32:
 * - Block data: 32 * 32 * 32 * 4 = 128 KB per chunk
 *
 * @section thread_safety Thread Safety
 *
 * Chunk is not thread-safe. External synchronization is required for
 * concurrent access.
 */
class Chunk {
public:
    /// Chunk dimension (blocks per side)
    static constexpr int SIZE = ChunkSize;

    /// Total blocks in chunk
    static constexpr int VOLUME = SIZE * SIZE * SIZE;

    /**
     * @brief Construct an empty chunk at origin.
     */
    Chunk();

    /**
     * @brief Construct an empty chunk at the specified position.
     * @param position The chunk coordinate
     */
    explicit Chunk(ChunkCoord position);

    /// Get chunk position in chunk coordinates
    ChunkCoord position() const { return m_position; }

    /**
     * @brief Get block at local coordinates.
     *
     * @param x Local X (0 to SIZE-1)
     * @param y Local Y (0 to SIZE-1)
     * @param z Local Z (0 to SIZE-1)
     * @return The block state
     *
     * @note Coordinates are not bounds-checked in release builds.
     */
    BlockState getBlock(int x, int y, int z) const;

    /**
     * @brief Set block at local coordinates.
     *
     * Automatically marks the chunk as dirty and updates internal counters.
     *
     * @param x Local X (0 to SIZE-1)
     * @param y Local Y (0 to SIZE-1)
     * @param z Local Z (0 to SIZE-1)
     * @param state The new block state
     */
    void setBlock(int x, int y, int z, BlockState state);

    /**
     * @brief Fill entire chunk with a single block state.
     * @param state The block state to fill with
     */
    void fill(BlockState state);

    /**
     * @brief Copy block data from a span.
     *
     * @param data Span of exactly VOLUME BlockState values
     * @throws std::invalid_argument if data.size() != VOLUME
     */
    void copyFrom(std::span<const BlockState> data);

    /// @name State Tracking
    /// @{

    /// Check if chunk needs mesh rebuild
    bool isDirty() const { return m_dirty; }

    /// Clear dirty flag (after mesh rebuild)
    void clearDirty() { m_dirty = false; }

    /// Mark chunk as needing mesh rebuild
    void markDirty() { m_dirty = true; }

    /// Check if chunk contains only air blocks
    bool isEmpty() const { return m_nonAirCount == 0; }

    /// Check if chunk is completely filled with opaque blocks
    bool isFullyOpaque() const { return m_opaqueCount == VOLUME; }

    /// Get count of non-air blocks
    uint32_t nonAirCount() const { return m_nonAirCount; }

    /// @}

    /**
     * @brief Get read-only access to raw block data.
     *
     * Useful for mesh generation where all blocks need to be accessed.
     *
     * @return Reference to internal block array
     */
    const std::array<BlockState, VOLUME>& blocks() const { return m_blocks; }

    /// @name Serialization
    /// @{

    /**
     * @brief Serialize chunk to binary format.
     * @return Binary data suitable for storage
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize chunk from binary format.
     * @param data Binary data from serialize()
     * @return The deserialized chunk
     * @throws std::runtime_error on invalid data
     */
    static Chunk deserialize(std::span<const uint8_t> data);

    /// @}

private:
    ChunkCoord m_position{0, 0, 0};
    std::array<BlockState, VOLUME> m_blocks{};

    // Cached state
    bool m_dirty = true;
    uint32_t m_nonAirCount = 0;
    uint32_t m_opaqueCount = 0;

    /// Convert 3D coordinates to flat array index
    static constexpr int flatIndex(int x, int y, int z) {
        return x + y * SIZE + z * SIZE * SIZE;
    }

    /// Update counters when a block changes
    void updateCounters(BlockState oldState, BlockState newState);
};

} // namespace Rigel::Voxel
