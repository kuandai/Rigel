#include "Rigel/Voxel/ChunkCache.h"

#include <iterator>

namespace Rigel::Voxel {

void ChunkCache::setMaxChunks(size_t maxChunks) {
    const size_t previousMaxChunks = m_maxChunks;
    m_maxChunks = maxChunks;
    if (m_maxChunks == 0 || m_entries.size() <= m_maxChunks) {
        m_evictionScanActive = false;
        m_evictionRescanRequested = false;
    } else if (m_maxChunks != previousMaxChunks) {
        requestEviction();
    }
}

void ChunkCache::touch(ChunkCoord coord) {
    auto it = m_entries.find(coord);
    if (it != m_entries.end()) {
        if (m_evictionScanActive && it->second == m_evictionCursor) {
            m_evictionCursor = std::next(it->second);
        }
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
    if (m_evictionScanActive && it->second == m_evictionCursor) {
        m_evictionCursor = std::next(it->second);
    }
    m_lru.erase(it->second);
    m_entries.erase(it);
    if (m_maxChunks == 0 || m_entries.size() <= m_maxChunks) {
        m_evictionScanActive = false;
        m_evictionRescanRequested = false;
    }
}

void ChunkCache::requestEviction() {
    if (m_maxChunks == 0 || m_entries.size() <= m_maxChunks) {
        m_evictionScanActive = false;
        m_evictionRescanRequested = false;
        return;
    }
    if (m_evictionScanActive) {
        m_evictionRescanRequested = true;
        return;
    }
    m_evictionCursor = m_lru.end();
    m_evictionScanActive = true;
    m_evictionRescanRequested = false;
}

std::vector<ChunkCoord> ChunkCache::evict(
    const std::unordered_set<ChunkCoord, ChunkCoordHash>& protectedSet,
    const std::function<bool(ChunkCoord)>& canEvict,
    size_t inspectionBudget
) {
    std::vector<ChunkCoord> evicted;
    m_lastEvictionInspections = 0;
    if (!m_evictionScanActive) {
        requestEviction();
    }
    if (!m_evictionScanActive || inspectionBudget == 0) {
        return evicted;
    }

    while (m_evictionScanActive &&
           m_lastEvictionInspections < inspectionBudget) {
        if (m_entries.size() <= m_maxChunks) {
            m_evictionScanActive = false;
            m_evictionRescanRequested = false;
            break;
        }
        if (m_evictionCursor == m_lru.begin()) {
            if (!m_evictionRescanRequested) {
                m_evictionScanActive = false;
                break;
            }
            m_evictionCursor = m_lru.end();
            m_evictionRescanRequested = false;
            continue;
        }

        auto candidate = std::prev(m_evictionCursor);
        ++m_lastEvictionInspections;
        ChunkCoord coord = *candidate;
        m_evictionCursor = candidate;
        if (protectedSet.find(coord) != protectedSet.end()) {
            continue;
        }
        if (canEvict && !canEvict(coord)) {
            continue;
        }

        m_entries.erase(coord);
        m_evictionCursor = m_lru.erase(candidate);
        evicted.push_back(coord);
    }

    if (m_evictionScanActive && m_entries.size() <= m_maxChunks) {
        m_evictionScanActive = false;
        m_evictionRescanRequested = false;
    } else if (m_evictionScanActive &&
               m_evictionCursor == m_lru.begin()) {
        if (m_evictionRescanRequested) {
            m_evictionCursor = m_lru.end();
            m_evictionRescanRequested = false;
        } else {
            m_evictionScanActive = false;
        }
    }

    return evicted;
}

} // namespace Rigel::Voxel
