#pragma once

#include "ChunkCoord.h"

#include <functional>
#include <list>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rigel::Voxel {

class ChunkStreamer;

class ChunkCache {
public:
    void setMaxChunks(size_t maxChunks);
    size_t maxChunks() const { return m_maxChunks; }

    void touch(ChunkCoord coord);
    void erase(ChunkCoord coord);

    // Evicts least-recently-used unprotected entries. Protected entries keep
    // their recency and may leave the cache above the configured limit. A
    // finite inspection budget advances the active pass across calls.
    std::vector<ChunkCoord> evict(
        const std::unordered_set<ChunkCoord, ChunkCoordHash>& protectedSet,
        const std::function<bool(ChunkCoord)>& canEvict = {},
        size_t inspectionBudget = std::numeric_limits<size_t>::max());

    size_t size() const { return m_entries.size(); }
    size_t lastEvictionInspections() const { return m_lastEvictionInspections; }

private:
    friend class ChunkStreamer;

    // Starts one LRU pass, coalescing another request made while that pass is
    // active into at most one follow-up pass.
    void requestEviction();
    bool evictionPending() const { return m_evictionScanActive; }

    size_t m_maxChunks = 0;
    size_t m_lastEvictionInspections = 0;
    std::list<ChunkCoord> m_lru;
    std::unordered_map<ChunkCoord, std::list<ChunkCoord>::iterator, ChunkCoordHash> m_entries;
    std::list<ChunkCoord>::iterator m_evictionCursor;
    bool m_evictionScanActive = false;
    bool m_evictionRescanRequested = false;
};

} // namespace Rigel::Voxel
