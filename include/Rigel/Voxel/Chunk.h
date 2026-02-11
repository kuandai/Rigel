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
#include <memory>
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
class BlockRegistry;

class Chunk {
public:
    /// Chunk dimension (blocks per side)
    static constexpr int SIZE = ChunkSize;

    /// Total blocks in chunk
    static constexpr int VOLUME = SIZE * SIZE * SIZE;

    /// Subchunk dimension (blocks per side)
    static constexpr int SUBCHUNK_SIZE = SIZE / 2;

    /// Total blocks per subchunk
    static constexpr int SUBCHUNK_VOLUME = SUBCHUNK_SIZE * SUBCHUNK_SIZE * SUBCHUNK_SIZE;

    /// Total subchunks per chunk (2x2x2)
    static constexpr int SUBCHUNK_COUNT = 8;

    static_assert(SIZE % 2 == 0, "Chunk SIZE must be divisible by 2");

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
     * @brief Set block with registry-driven opacity tracking.
     */
    void setBlock(int x, int y, int z, BlockState state, const BlockRegistry& registry);

    /**
     * @brief Fill entire chunk with a single block state.
     * @param state The block state to fill with
     */
    void fill(BlockState state);

    /**
     * @brief Fill entire chunk with a single block state (opacity-aware).
     */
    void fill(BlockState state, const BlockRegistry& registry);

    /**
     * @brief Copy block data from a span.
     *
     * @param data Span of exactly VOLUME BlockState values
     * @throws std::invalid_argument if data.size() != VOLUME
     */
    void copyFrom(std::span<const BlockState> data);

    /**
     * @brief Copy block data from a span (opacity-aware).
     */
    void copyFrom(std::span<const BlockState> data, const BlockRegistry& registry);

    /// @name State Tracking
    /// @{

    /// Check if chunk needs mesh rebuild
    bool isDirty() const { return m_dirty; }

    /// Check if chunk needs persistence write
    bool isPersistDirty() const { return m_persistDirty; }

    /// Check if chunk was loaded from persistence
    bool loadedFromDisk() const { return m_loadedFromDisk; }

    /// Clear dirty flag (after mesh rebuild)
    void clearDirty() { m_dirty = false; }

    /// Clear persistence dirty flag (after save or load)
    void clearPersistDirty() { m_persistDirty = false; }

    /// Mark whether the chunk data originated from persistence
    void setLoadedFromDisk(bool loaded) { m_loadedFromDisk = loaded; }

    /// Mark chunk as needing mesh rebuild
    void markDirty() {
        m_dirty = true;
        bumpMeshRevision();
    }

    /// Mark chunk as needing persistence write
    void markPersistDirty() { m_persistDirty = true; }

    /// Check if chunk contains only air blocks
    bool isEmpty() const { return m_nonAirCount == 0; }

    /// Check if chunk is completely filled with opaque blocks
    bool isFullyOpaque() const { return m_opaqueCount == VOLUME; }

    /// Get count of non-air blocks
    uint32_t nonAirCount() const { return m_nonAirCount; }

    /// Get count of opaque blocks
    uint32_t opaqueCount() const { return m_opaqueCount; }

    /// Mesh revision for tracking stale mesh tasks
    uint32_t meshRevision() const { return m_meshRevision; }

    /// Get worldgen version metadata
    uint32_t worldGenVersion() const { return m_worldGenVersion; }

    /// Set worldgen version metadata
    void setWorldGenVersion(uint32_t version) { m_worldGenVersion = version; }

    /// @}

    /**
     * @brief Copy all blocks into an output span.
     *
     * The output span must be VOLUME in size.
     */
    void copyBlocks(std::span<BlockState> out) const;

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
    struct Subchunk {
        std::unique_ptr<std::array<BlockState, SUBCHUNK_VOLUME>> blocks;
        uint32_t nonAirCount = 0;
        uint32_t opaqueCount = 0;

        bool isAllocated() const { return blocks != nullptr; }
        void allocate();
        void clear();
    };

    ChunkCoord m_position{0, 0, 0};
    std::array<Subchunk, SUBCHUNK_COUNT> m_subchunks{};

    // Cached state
    bool m_dirty = true;
    bool m_persistDirty = false;
    bool m_loadedFromDisk = false;
    uint32_t m_nonAirCount = 0;
    uint32_t m_opaqueCount = 0;
    uint32_t m_meshRevision = 0;
    uint32_t m_worldGenVersion = 0;

    /// Convert 3D coordinates to flat array index
    static constexpr int flatIndex(int x, int y, int z) {
        return x + y * SIZE + z * SIZE * SIZE;
    }

    static constexpr int subchunkIndex(int x, int y, int z) {
        return (x / SUBCHUNK_SIZE) + (y / SUBCHUNK_SIZE) * 2 + (z / SUBCHUNK_SIZE) * 4;
    }

    static constexpr int subchunkLocal(int value) {
        return value % SUBCHUNK_SIZE;
    }

    static constexpr int subchunkFlatIndex(int x, int y, int z) {
        return x + y * SUBCHUNK_SIZE + z * SUBCHUNK_SIZE * SUBCHUNK_SIZE;
    }

    void bumpMeshRevision() {
        uint32_t next = m_meshRevision + 1;
        m_meshRevision = (next == 0) ? 1 : next;
    }

    void setBlockInternal(int x, int y, int z, BlockState state, const BlockRegistry* registry);
    void fillInternal(BlockState state, const BlockRegistry* registry);
    void copyFromInternal(std::span<const BlockState> data, const BlockRegistry* registry);
};

} // namespace Rigel::Voxel
