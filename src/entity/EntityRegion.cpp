#include "Rigel/Entity/EntityRegion.h"

namespace Rigel::Entity {

namespace {
int floorDiv(int a, int b) {
    return (a >= 0) ? (a / b) : ((a - b + 1) / b);
}
} // namespace

EntityRegionCoord chunkToRegion(Voxel::ChunkCoord coord) {
    return EntityRegionCoord{
        floorDiv(coord.x, EntityRegionChunkSpan),
        floorDiv(coord.y, EntityRegionChunkSpan),
        floorDiv(coord.z, EntityRegionChunkSpan)
    };
}

EntityRegion::EntityRegion(EntityRegionCoord coord)
    : m_coord(coord)
{}

EntityChunk* EntityRegion::findActiveChunk(Voxel::ChunkCoord coord) {
    auto it = m_activeChunks.find(coord);
    if (it == m_activeChunks.end()) {
        return nullptr;
    }
    return it->second.get();
}

EntityChunk& EntityRegion::getOrActivateChunk(Voxel::ChunkCoord coord) {
    if (auto* active = findActiveChunk(coord)) {
        return *active;
    }
    auto inactiveIt = m_inactiveChunks.find(coord);
    if (inactiveIt != m_inactiveChunks.end()) {
        std::unique_ptr<EntityChunk> chunk = std::move(inactiveIt->second);
        m_inactiveChunks.erase(inactiveIt);
        chunk->setRegion(this);
        auto [it, _] = m_activeChunks.emplace(coord, std::move(chunk));
        return *it->second;
    }
    auto chunk = std::make_unique<EntityChunk>(coord);
    chunk->setRegion(this);
    auto [it, _] = m_activeChunks.emplace(coord, std::move(chunk));
    return *it->second;
}

void EntityRegion::deactivateChunk(Voxel::ChunkCoord coord) {
    auto it = m_activeChunks.find(coord);
    if (it == m_activeChunks.end()) {
        return;
    }
    m_inactiveChunks.emplace(coord, std::move(it->second));
    m_activeChunks.erase(it);
}

bool EntityRegion::isEmpty() const {
    return m_activeChunks.empty() && m_inactiveChunks.empty();
}

} // namespace Rigel::Entity
