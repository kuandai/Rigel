#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/ChunkManager.h"

#include <cassert>
#include <stdexcept>
#include <algorithm>

namespace Rigel::Voxel {

void Chunk::bumpMeshRevision() {
    uint32_t next = m_meshRevision + 1;
    m_meshRevision = (next == 0) ? 1 : next;
    notifyMeshChange();
}

void Chunk::notifyMeshChange() {
    if (m_chunkManager) {
        m_chunkManager->notifyMeshChange(m_position);
    }
}

void Chunk::trackMeshChanges(ChunkManager* manager) {
    m_chunkManager = manager;
}

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

    m_persistDirty = true;
    invalidateMesh();
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

    m_persistDirty = true;
    invalidateMesh();
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
        m_persistDirty = true;
        invalidateMesh();
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
    m_persistDirty = true;
    invalidateMesh();
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
