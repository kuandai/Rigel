#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

void ChunkManager::invalidateFaceNeighbors(ChunkCoord coord) {
    for (size_t i = 0; i < DirectionCount; ++i) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(i), dx, dy, dz);
        if (Chunk* neighbor = getChunk(coord.offset(dx, dy, dz));
            neighbor && !neighbor->isEmpty()) {
            neighbor->invalidateMesh();
        }
    }
}

void ChunkManager::notifyMeshChange(ChunkCoord coord) {
    if (m_dirtyMeshQueued.insert(coord).second) {
        m_dirtyMeshQueue.push_back(coord);
    }
}

std::vector<ChunkCoord> ChunkManager::consumeDirtyMeshNotifications() {
    std::vector<ChunkCoord> dirty;
    dirty.reserve(m_dirtyMeshQueued.size());
    while (!m_dirtyMeshQueue.empty()) {
        ChunkCoord coord = m_dirtyMeshQueue.front();
        m_dirtyMeshQueue.pop_front();
        if (m_dirtyMeshQueued.erase(coord) != 0) {
            dirty.push_back(coord);
        }
    }
    return dirty;
}

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
    chunk->trackMeshChanges(this);
    Chunk& ref = *chunk;
    m_chunks[coord] = std::move(chunk);
    notifyMeshChange(coord);

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
            neighbor->invalidateMesh();
        }
    } else if (lx == Chunk::SIZE - 1) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(1, 0, 0))) {
            neighbor->invalidateMesh();
        }
    }

    if (ly == 0) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, -1, 0))) {
            neighbor->invalidateMesh();
        }
    } else if (ly == Chunk::SIZE - 1) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, 1, 0))) {
            neighbor->invalidateMesh();
        }
    }

    if (lz == 0) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, 0, -1))) {
            neighbor->invalidateMesh();
        }
    } else if (lz == Chunk::SIZE - 1) {
        if (Chunk* neighbor = getChunk(chunkCoord.offset(0, 0, 1))) {
            neighbor->invalidateMesh();
        }
    }
}

void ChunkManager::unloadChunk(ChunkCoord coord, bool invalidateNeighbors) {
    auto it = m_chunks.find(coord);
    if (it != m_chunks.end()) {
        if (invalidateNeighbors) {
            invalidateFaceNeighbors(coord);
        }
        m_chunks.erase(it);
        m_dirtyMeshQueued.erase(coord);
        spdlog::debug("Unloaded chunk at ({}, {}, {})", coord.x, coord.y, coord.z);
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
