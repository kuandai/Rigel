#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <cassert>
#include <stdexcept>
#include <cstring>
#include <algorithm>

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

    int index = subchunkIndex(x, y, z);
    const Subchunk& subchunk = m_subchunks[index];
    if (!subchunk.blocks) {
        return BlockState{};
    }

    int lx = subchunkLocal(x);
    int ly = subchunkLocal(y);
    int lz = subchunkLocal(z);
    return (*subchunk.blocks)[subchunkFlatIndex(lx, ly, lz)];
}

void Chunk::setBlock(int x, int y, int z, BlockState state) {
    setBlockInternal(x, y, z, state, nullptr);
}

void Chunk::setBlock(int x, int y, int z, BlockState state, const BlockRegistry& registry) {
    setBlockInternal(x, y, z, state, &registry);
}

void Chunk::setBlockInternal(int x, int y, int z, BlockState state, const BlockRegistry* registry) {
    assert(x >= 0 && x < SIZE);
    assert(y >= 0 && y < SIZE);
    assert(z >= 0 && z < SIZE);

    int index = subchunkIndex(x, y, z);
    Subchunk& subchunk = m_subchunks[index];

    if (!subchunk.blocks) {
        if (state.isAir()) {
            return;
        }
        subchunk.allocate();
    }

    int lx = subchunkLocal(x);
    int ly = subchunkLocal(y);
    int lz = subchunkLocal(z);
    int localIndex = subchunkFlatIndex(lx, ly, lz);
    BlockState oldState = (*subchunk.blocks)[localIndex];

    if (oldState == state) {
        return;  // No change
    }

    bool oldNonAir = !oldState.isAir();
    bool newNonAir = !state.isAir();
    bool oldOpaque = false;
    bool newOpaque = false;
    if (registry) {
        if (oldNonAir) {
            oldOpaque = registry->getType(oldState.id).isOpaque;
        }
        if (newNonAir) {
            newOpaque = registry->getType(state.id).isOpaque;
        }
    }

    (*subchunk.blocks)[localIndex] = state;

    if (oldNonAir != newNonAir) {
        int delta = newNonAir ? 1 : -1;
        m_nonAirCount = static_cast<uint32_t>(static_cast<int>(m_nonAirCount) + delta);
        subchunk.nonAirCount = static_cast<uint32_t>(static_cast<int>(subchunk.nonAirCount) + delta);
    }

    if (registry && oldOpaque != newOpaque) {
        int delta = newOpaque ? 1 : -1;
        m_opaqueCount = static_cast<uint32_t>(static_cast<int>(m_opaqueCount) + delta);
        subchunk.opaqueCount = static_cast<uint32_t>(static_cast<int>(subchunk.opaqueCount) + delta);
    }

    if (subchunk.nonAirCount == 0) {
        subchunk.clear();
    }

    m_dirty = true;
    bumpMeshRevision();
}

void Chunk::fill(BlockState state) {
    fillInternal(state, nullptr);
}

void Chunk::fill(BlockState state, const BlockRegistry& registry) {
    fillInternal(state, &registry);
}

void Chunk::copyFrom(std::span<const BlockState> data) {
    copyFromInternal(data, nullptr);
}

void Chunk::copyFrom(std::span<const BlockState> data, const BlockRegistry& registry) {
    copyFromInternal(data, &registry);
}

void Chunk::copyFromInternal(std::span<const BlockState> data, const BlockRegistry* registry) {
    if (data.size() != VOLUME) {
        throw std::invalid_argument(
            "Chunk::copyFrom: expected " + std::to_string(VOLUME) +
            " blocks, got " + std::to_string(data.size())
        );
    }

    for (Subchunk& subchunk : m_subchunks) {
        subchunk.clear();
    }

    m_nonAirCount = 0;
    m_opaqueCount = 0;

    for (int z = 0; z < SIZE; ++z) {
        for (int y = 0; y < SIZE; ++y) {
            for (int x = 0; x < SIZE; ++x) {
                BlockState state = data[flatIndex(x, y, z)];
                if (state.isAir()) {
                    continue;
                }

                int index = subchunkIndex(x, y, z);
                Subchunk& subchunk = m_subchunks[index];
                if (!subchunk.blocks) {
                    subchunk.allocate();
                }

                int lx = subchunkLocal(x);
                int ly = subchunkLocal(y);
                int lz = subchunkLocal(z);
                (*subchunk.blocks)[subchunkFlatIndex(lx, ly, lz)] = state;
                ++subchunk.nonAirCount;
                ++m_nonAirCount;

                if (registry && registry->getType(state.id).isOpaque) {
                    ++subchunk.opaqueCount;
                    ++m_opaqueCount;
                }
            }
        }
    }

    m_dirty = true;
    bumpMeshRevision();
}

std::vector<uint8_t> Chunk::serialize() const {
    // Simple format: magic + position + worldgen version + raw block data
    // Format: "RCHK" (4) + x,y,z (12) + worldgen (4) + blocks (VOLUME * 4)
    constexpr size_t headerSize = 4 + 12 + 4;
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
    std::memcpy(data.data() + 16, &m_worldGenVersion, 4);

    // Block data
    std::vector<BlockState> blocks(VOLUME);
    copyBlocks(std::span<BlockState>(blocks));
    std::memcpy(data.data() + headerSize, blocks.data(), blockDataSize);

    return data;
}

Chunk Chunk::deserialize(std::span<const uint8_t> data) {
    constexpr size_t headerSize = 4 + 12 + 4;
    constexpr size_t legacyHeaderSize = 4 + 12;
    constexpr size_t blockDataSize = VOLUME * sizeof(BlockState);
    constexpr size_t expectedSize = headerSize + blockDataSize;
    constexpr size_t legacySize = legacyHeaderSize + blockDataSize;

    if (data.size() != expectedSize && data.size() != legacySize) {
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
    if (data.size() >= headerSize) {
        std::memcpy(&chunk.m_worldGenVersion, data.data() + 16, 4);
    }

    // Read block data
    std::array<BlockState, VOLUME> blocks{};
    size_t dataOffset = (data.size() == legacySize) ? legacyHeaderSize : headerSize;
    std::memcpy(blocks.data(), data.data() + dataOffset, blockDataSize);
    chunk.copyFrom(blocks);

    return chunk;
}

void Chunk::copyBlocks(std::span<BlockState> out) const {
    if (out.size() != VOLUME) {
        throw std::invalid_argument(
            "Chunk::copyBlocks: expected " + std::to_string(VOLUME) +
            " blocks, got " + std::to_string(out.size())
        );
    }

    std::fill(out.begin(), out.end(), BlockState{});

    for (int sz = 0; sz < 2; ++sz) {
        for (int sy = 0; sy < 2; ++sy) {
            for (int sx = 0; sx < 2; ++sx) {
                int index = sx + sy * 2 + sz * 4;
                const Subchunk& subchunk = m_subchunks[index];
                if (!subchunk.blocks) {
                    continue;
                }

                for (int z = 0; z < SUBCHUNK_SIZE; ++z) {
                    for (int y = 0; y < SUBCHUNK_SIZE; ++y) {
                        for (int x = 0; x < SUBCHUNK_SIZE; ++x) {
                            int gx = sx * SUBCHUNK_SIZE + x;
                            int gy = sy * SUBCHUNK_SIZE + y;
                            int gz = sz * SUBCHUNK_SIZE + z;
                            out[flatIndex(gx, gy, gz)] =
                                (*subchunk.blocks)[subchunkFlatIndex(x, y, z)];
                        }
                    }
                }
            }
        }
    }
}

void Chunk::fillInternal(BlockState state, const BlockRegistry* registry) {
    for (Subchunk& subchunk : m_subchunks) {
        subchunk.clear();
    }

    if (state.isAir()) {
        m_nonAirCount = 0;
        m_opaqueCount = 0;
        m_dirty = true;
        bumpMeshRevision();
        return;
    }

    bool isOpaque = false;
    if (registry) {
        isOpaque = registry->getType(state.id).isOpaque;
    }

    for (Subchunk& subchunk : m_subchunks) {
        subchunk.allocate();
        subchunk.blocks->fill(state);
        subchunk.nonAirCount = SUBCHUNK_VOLUME;
        subchunk.opaqueCount = isOpaque ? SUBCHUNK_VOLUME : 0;
    }

    m_nonAirCount = VOLUME;
    m_opaqueCount = isOpaque ? VOLUME : 0;
    m_dirty = true;
    bumpMeshRevision();
}

void Chunk::Subchunk::allocate() {
    if (!blocks) {
        blocks = std::make_unique<std::array<BlockState, SUBCHUNK_VOLUME>>();
        blocks->fill(BlockState{});
    }
}

void Chunk::Subchunk::clear() {
    blocks.reset();
    nonAirCount = 0;
    opaqueCount = 0;
}

} // namespace Rigel::Voxel
