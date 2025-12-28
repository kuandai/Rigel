#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

Chunk* ChunkManager::getChunk(ChunkCoord coord) {
    auto it = m_chunks.find(coord);
    if (it == m_chunks.end()) {
        return nullptr;
    }
    return it->second.get();
}

const Chunk* ChunkManager::getChunk(ChunkCoord coord) const {
    auto it = m_chunks.find(coord);
    if (it == m_chunks.end()) {
        return nullptr;
    }
    return it->second.get();
}

Chunk& ChunkManager::getOrCreateChunk(ChunkCoord coord) {
    auto it = m_chunks.find(coord);
    if (it != m_chunks.end()) {
        return *it->second;
    }

    auto chunk = std::make_unique<Chunk>(coord);
    Chunk& ref = *chunk;
    m_chunks[coord] = std::move(chunk);

    spdlog::debug("Created chunk at ({}, {}, {})", coord.x, coord.y, coord.z);

    return ref;
}

bool ChunkManager::hasChunk(ChunkCoord coord) const {
    return m_chunks.find(coord) != m_chunks.end();
}

BlockState ChunkManager::getBlock(int wx, int wy, int wz) const {
    ChunkCoord chunkCoord = worldToChunk(wx, wy, wz);
    const Chunk* chunk = getChunk(chunkCoord);

    if (!chunk) {
        // Return air for unloaded chunks
        return BlockState{};
    }

    int lx, ly, lz;
    worldToLocal(wx, wy, wz, lx, ly, lz);

    return chunk->getBlock(lx, ly, lz);
}

void ChunkManager::setBlock(int wx, int wy, int wz, BlockState state) {
    ChunkCoord chunkCoord = worldToChunk(wx, wy, wz);
    Chunk& chunk = getOrCreateChunk(chunkCoord);

    int lx, ly, lz;
    worldToLocal(wx, wy, wz, lx, ly, lz);

    if (chunk.getBlock(lx, ly, lz) == state) {
        return;
    }
    if (m_registry) {
        chunk.setBlock(lx, ly, lz, state, *m_registry);
    } else {
        chunk.setBlock(lx, ly, lz, state);
    }

    if (lx == 0) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(-1, 0, 0))) {
            neighbor->markDirty();
        }
    } else if (lx == Chunk::SIZE - 1) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(1, 0, 0))) {
            neighbor->markDirty();
        }
    }

    if (ly == 0) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, -1, 0))) {
            neighbor->markDirty();
        }
    } else if (ly == Chunk::SIZE - 1) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, 1, 0))) {
            neighbor->markDirty();
        }
    }

    if (lz == 0) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, 0, -1))) {
            neighbor->markDirty();
        }
    } else if (lz == Chunk::SIZE - 1) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, 0, 1))) {
            neighbor->markDirty();
        }
    }
}

void ChunkManager::loadChunk(ChunkCoord coord, std::span<const uint8_t> data) {
    Chunk chunk = Chunk::deserialize(data);

    // Override position from coordinate (in case data has wrong position)
    // Note: This requires making a new chunk since position is set in constructor
    auto newChunk = std::make_unique<Chunk>(coord);
    std::array<BlockState, Chunk::VOLUME> blocks{};
    chunk.copyBlocks(blocks);
    if (m_registry) {
        newChunk->copyFrom(blocks, *m_registry);
    } else {
        newChunk->copyFrom(blocks);
    }

    m_chunks[coord] = std::move(newChunk);

    spdlog::debug("Loaded chunk at ({}, {}, {})", coord.x, coord.y, coord.z);
}

void ChunkManager::unloadChunk(ChunkCoord coord) {
    auto it = m_chunks.find(coord);
    if (it != m_chunks.end()) {
        m_chunks.erase(it);
        spdlog::debug("Unloaded chunk at ({}, {}, {})", coord.x, coord.y, coord.z);
    }
}

void ChunkManager::clear() {
    m_chunks.clear();
    spdlog::debug("ChunkManager cleared");
}

std::vector<ChunkCoord> ChunkManager::getDirtyChunks() const {
    std::vector<ChunkCoord> dirty;

    for (const auto& [coord, chunk] : m_chunks) {
        if (chunk->isDirty()) {
            dirty.push_back(coord);
        }
    }

    return dirty;
}

void ChunkManager::clearDirtyFlags() {
    for (auto& [coord, chunk] : m_chunks) {
        chunk->clearDirty();
    }
}

void ChunkManager::forEachChunk(const std::function<void(ChunkCoord, Chunk&)>& fn) {
    for (auto& [coord, chunk] : m_chunks) {
        fn(coord, *chunk);
    }
}

void ChunkManager::forEachChunk(const std::function<void(ChunkCoord, const Chunk&)>& fn) const {
    for (const auto& [coord, chunk] : m_chunks) {
        fn(coord, *chunk);
    }
}

} // namespace Rigel::Voxel
