#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

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
    auto [entry, inserted] = m_dirtyMeshQueued.insert(coord);
    if (inserted) {
        try {
            m_dirtyMeshQueue.push_back(coord);
        } catch (...) {
            m_dirtyMeshQueued.erase(entry);
            throw;
        }
    }
}

std::vector<ChunkCoord> ChunkManager::consumeDirtyMeshNotifications() {
    std::vector<ChunkCoord> dirty;
    dirty.swap(m_dirtyMeshQueue);
    for (const ChunkCoord coord : dirty) m_dirtyMeshQueued.erase(coord);
    return dirty;
}

ChunkManager::PreparedChunkReplacements::PreparedChunkReplacements(
    PreparedChunkReplacements&& other
) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr)),
      m_replacements(std::move(other.m_replacements)),
      m_insertedSlots(std::move(other.m_insertedSlots)),
      m_neighborInvalidations(std::move(other.m_neighborInvalidations)),
      m_insertedNotifications(std::move(other.m_insertedNotifications)) {
}

ChunkManager::PreparedChunkReplacements&
ChunkManager::PreparedChunkReplacements::operator=(
    PreparedChunkReplacements&& other
) noexcept {
    if (this == &other) return *this;
    discard();
    m_owner = std::exchange(other.m_owner, nullptr);
    m_replacements = std::move(other.m_replacements);
    m_insertedSlots = std::move(other.m_insertedSlots);
    m_neighborInvalidations = std::move(other.m_neighborInvalidations);
    m_insertedNotifications = std::move(other.m_insertedNotifications);
    return *this;
}

ChunkManager::PreparedChunkReplacements::~PreparedChunkReplacements() {
    discard();
}

void ChunkManager::PreparedChunkReplacements::discard() noexcept {
    if (!m_owner) return;
    for (const ChunkCoord coord : m_insertedSlots) {
        const auto found = m_owner->m_chunks.find(coord);
        if (found != m_owner->m_chunks.end() && !found->second) {
            m_owner->m_chunks.erase(found);
        }
    }
    for (const ChunkCoord coord : m_insertedNotifications) {
        m_owner->m_dirtyMeshQueued.erase(coord);
    }
    m_owner = nullptr;
}

ChunkManager::PreparedChunkReplacements
ChunkManager::prepareChunkReplacements(
    std::vector<std::unique_ptr<Chunk>> replacements
) {
    std::sort(replacements.begin(), replacements.end(),
              [](const auto& first, const auto& second) {
                  if (!first) return static_cast<bool>(second);
                  if (!second) return false;
                  return first->position() < second->position();
              });
    for (size_t index = 0; index < replacements.size(); ++index) {
        if (!replacements[index] ||
            (index != 0 && replacements[index - 1]->position() ==
                               replacements[index]->position())) {
            throw std::invalid_argument("invalid prepared chunk replacement");
        }
    }

    PreparedChunkReplacements prepared;
    prepared.m_replacements = std::move(replacements);
    prepared.m_insertedSlots.reserve(prepared.m_replacements.size());
    prepared.m_neighborInvalidations.reserve(
        prepared.m_replacements.size() * DirectionCount);
    prepared.m_insertedNotifications.reserve(
        prepared.m_replacements.size() * (DirectionCount + 1));

    const auto isReplacement = [&](ChunkCoord coord) {
        const auto found = std::lower_bound(
            prepared.m_replacements.begin(), prepared.m_replacements.end(),
            coord, [](const auto& entry, ChunkCoord value) {
                return entry->position() < value;
            });
        return found != prepared.m_replacements.end() &&
            (*found)->position() == coord;
    };
    const auto appendNeighbor = [&](ChunkCoord coord) {
        if (isReplacement(coord) || !getChunk(coord) ||
            std::find(prepared.m_neighborInvalidations.begin(),
                      prepared.m_neighborInvalidations.end(), coord) !=
                prepared.m_neighborInvalidations.end()) {
            return;
        }
        prepared.m_neighborInvalidations.push_back(coord);
    };
    for (const auto& replacement : prepared.m_replacements) {
        const ChunkCoord coord = replacement->position();
        for (size_t direction = 0; direction < DirectionCount; ++direction) {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(
                static_cast<Direction>(direction), dx, dy, dz);
            appendNeighbor(coord.offset(dx, dy, dz));
        }
    }

    m_chunks.reserve(m_chunks.size() + prepared.m_replacements.size());
    m_dirtyMeshQueue.reserve(
        m_dirtyMeshQueue.size() + prepared.m_replacements.size() +
        prepared.m_neighborInvalidations.size());
    try {
        const auto prepareNotification = [&](ChunkCoord coord) {
            if (m_dirtyMeshQueued.insert(coord).second) {
                prepared.m_insertedNotifications.push_back(coord);
            }
        };
        for (const auto& replacement : prepared.m_replacements) {
            prepareNotification(replacement->position());
        }
        for (const ChunkCoord coord : prepared.m_neighborInvalidations) {
            prepareNotification(coord);
        }
        for (const auto& replacement : prepared.m_replacements) {
            const ChunkCoord coord = replacement->position();
            if (m_chunks.contains(coord)) continue;
            auto [_, inserted] = m_chunks.emplace(coord, nullptr);
            if (inserted) prepared.m_insertedSlots.push_back(coord);
        }
    } catch (...) {
        for (const ChunkCoord coord : prepared.m_insertedSlots) {
            m_chunks.erase(coord);
        }
        for (const ChunkCoord coord : prepared.m_insertedNotifications) {
            m_dirtyMeshQueued.erase(coord);
        }
        throw;
    }
    prepared.m_owner = this;
    return prepared;
}

void ChunkManager::installChunkReplacements(
    PreparedChunkReplacements prepared
) noexcept {
    if (prepared.m_owner != this) std::terminate();
    for (auto& replacement : prepared.m_replacements) {
        const ChunkCoord coord = replacement->position();
        auto found = m_chunks.find(coord);
        if (found == m_chunks.end()) std::terminate();
        found->second.swap(replacement);
        found->second->trackMeshChanges(this);
    }
    for (const ChunkCoord coord : prepared.m_neighborInvalidations) {
        if (Chunk* neighbor = getChunk(coord)) neighbor->invalidateMesh();
    }
    for (const ChunkCoord coord : prepared.m_insertedNotifications) {
        m_dirtyMeshQueue.push_back(coord);
    }
    prepared.m_owner = nullptr;
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
        m_dirtyMeshQueue.erase(
            std::remove(
                m_dirtyMeshQueue.begin(), m_dirtyMeshQueue.end(), coord),
            m_dirtyMeshQueue.end());
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
