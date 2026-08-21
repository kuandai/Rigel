#include "Rigel/Voxel/ChunkCache.h"

namespace Rigel::Voxel {

void ChunkCache::setMaxChunks(size_t maxChunks) {
    m_maxChunks = maxChunks;
}

void ChunkCache::touch(ChunkCoord coord) {
    auto it = m_entries.find(coord);
    if (it != m_entries.end()) {
        m_lru.erase(it->second);
        m_lru.push_front(coord);
        it->second = m_lru.begin();
        return;
    }

    m_lru.push_front(coord);
    m_entries.emplace(coord, m_lru.begin());
}

void ChunkCache::erase(ChunkCoord coord) {
    auto it = m_entries.find(coord);
    if (it == m_entries.end()) {
        return;
    }
    m_lru.erase(it->second);
    m_entries.erase(it);
}

std::vector<ChunkCoord> ChunkCache::evict(
    const std::unordered_set<ChunkCoord, ChunkCoordHash>& protectedSet,
    const std::function<bool(ChunkCoord)>& canEvict
) {
    std::vector<ChunkCoord> evicted;
    m_lastEvictionInspections = 0;
    if (m_maxChunks == 0) {
        return evicted;
    }

    auto it = m_lru.end();
    while (m_entries.size() > m_maxChunks && it != m_lru.begin()) {
        --it;
        ++m_lastEvictionInspections;
        ChunkCoord coord = *it;
        if (protectedSet.find(coord) != protectedSet.end()) {
            continue;
        }
        if (canEvict && !canEvict(coord)) {
            continue;
        }

        m_entries.erase(coord);
        it = m_lru.erase(it);
        evicted.push_back(coord);
    }

    return evicted;
}

} // namespace Rigel::Voxel
