#include "Rigel/Voxel/Chunk.h"

#include <cassert>
#include <stdexcept>
#include <cstring>

namespace Rigel::Voxel {

Chunk::Chunk() {
    // All blocks default to air (BlockState default constructor)
    // Counters already at 0
}

Chunk::Chunk(ChunkCoord position)
    : m_position(position)
{
    // All blocks default to air
}

BlockState Chunk::getBlock(int x, int y, int z) const {
    assert(x >= 0 && x < SIZE);
    assert(y >= 0 && y < SIZE);
    assert(z >= 0 && z < SIZE);
    return m_blocks[flatIndex(x, y, z)];
}

void Chunk::setBlock(int x, int y, int z, BlockState state) {
    assert(x >= 0 && x < SIZE);
    assert(y >= 0 && y < SIZE);
    assert(z >= 0 && z < SIZE);

    int idx = flatIndex(x, y, z);
    BlockState oldState = m_blocks[idx];

    if (oldState == state) {
        return;  // No change
    }

    m_blocks[idx] = state;
    updateCounters(oldState, state);
    m_dirty = true;
}

void Chunk::fill(BlockState state) {
    m_blocks.fill(state);

    // Update counters
    if (state.isAir()) {
        m_nonAirCount = 0;
        m_opaqueCount = 0;
    } else {
        m_nonAirCount = VOLUME;
        // Note: We don't have access to BlockRegistry here to check opacity
        // This will be recalculated when needed or set by caller
        m_opaqueCount = 0;  // Conservative default
    }

    m_dirty = true;
}

void Chunk::copyFrom(std::span<const BlockState> data) {
    if (data.size() != VOLUME) {
        throw std::invalid_argument(
            "Chunk::copyFrom: expected " + std::to_string(VOLUME) +
            " blocks, got " + std::to_string(data.size())
        );
    }

    std::copy(data.begin(), data.end(), m_blocks.begin());

    // Recalculate counters
    m_nonAirCount = 0;
    m_opaqueCount = 0;
    for (const BlockState& block : m_blocks) {
        if (!block.isAir()) {
            m_nonAirCount++;
        }
    }

    m_dirty = true;
}

void Chunk::updateCounters(BlockState oldState, BlockState newState) {
    // Update non-air count
    if (oldState.isAir() && !newState.isAir()) {
        m_nonAirCount++;
    } else if (!oldState.isAir() && newState.isAir()) {
        m_nonAirCount--;
    }

    // Note: Opaque count would need BlockRegistry access
    // For now, we don't track it dynamically
}

std::vector<uint8_t> Chunk::serialize() const {
    // Simple format: magic + position + raw block data
    // Format: "RCHK" (4) + x,y,z (12) + blocks (VOLUME * 4)
    constexpr size_t headerSize = 4 + 12;
    constexpr size_t blockDataSize = VOLUME * sizeof(BlockState);

    std::vector<uint8_t> data(headerSize + blockDataSize);

    // Magic
    data[0] = 'R';
    data[1] = 'C';
    data[2] = 'H';
    data[3] = 'K';

    // Position
    std::memcpy(data.data() + 4, &m_position.x, 4);
    std::memcpy(data.data() + 8, &m_position.y, 4);
    std::memcpy(data.data() + 12, &m_position.z, 4);

    // Block data
    std::memcpy(data.data() + headerSize, m_blocks.data(), blockDataSize);

    return data;
}

Chunk Chunk::deserialize(std::span<const uint8_t> data) {
    constexpr size_t headerSize = 4 + 12;
    constexpr size_t blockDataSize = VOLUME * sizeof(BlockState);
    constexpr size_t expectedSize = headerSize + blockDataSize;

    if (data.size() != expectedSize) {
        throw std::runtime_error(
            "Chunk::deserialize: invalid data size (expected " +
            std::to_string(expectedSize) + ", got " +
            std::to_string(data.size()) + ")"
        );
    }

    // Check magic
    if (data[0] != 'R' || data[1] != 'C' || data[2] != 'H' || data[3] != 'K') {
        throw std::runtime_error("Chunk::deserialize: invalid magic");
    }

    // Read position
    ChunkCoord position;
    std::memcpy(&position.x, data.data() + 4, 4);
    std::memcpy(&position.y, data.data() + 8, 4);
    std::memcpy(&position.z, data.data() + 12, 4);

    Chunk chunk(position);

    // Read block data
    std::memcpy(chunk.m_blocks.data(), data.data() + headerSize, blockDataSize);

    // Recalculate counters
    chunk.m_nonAirCount = 0;
    for (const BlockState& block : chunk.m_blocks) {
        if (!block.isAir()) {
            chunk.m_nonAirCount++;
        }
    }

    chunk.m_dirty = true;

    return chunk;
}

} // namespace Rigel::Voxel
