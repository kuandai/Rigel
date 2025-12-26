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
    const std::unordered_set<ChunkCoord, ChunkCoordHash>& protectedSet
) {
    std::vector<ChunkCoord> evicted;
    if (m_maxChunks == 0) {
        return evicted;
    }

    while (m_entries.size() > m_maxChunks && !m_lru.empty()) {
        ChunkCoord coord = m_lru.back();
        if (protectedSet.find(coord) != protectedSet.end()) {
            m_lru.pop_back();
            m_lru.push_front(coord);
            m_entries[coord] = m_lru.begin();
            continue;
        }

        m_lru.pop_back();
        m_entries.erase(coord);
        evicted.push_back(coord);
    }

    return evicted;
}

} // namespace Rigel::Voxel
