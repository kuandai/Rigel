#pragma once

#include "ChunkCoord.h"

#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rigel::Voxel {

class ChunkCache {
public:
    void setMaxChunks(size_t maxChunks);
    size_t maxChunks() const { return m_maxChunks; }

    void touch(ChunkCoord coord);
    void erase(ChunkCoord coord);

    std::vector<ChunkCoord> evict(const std::unordered_set<ChunkCoord, ChunkCoordHash>& protectedSet);

    size_t size() const { return m_entries.size(); }

private:
    size_t m_maxChunks = 0;
    std::list<ChunkCoord> m_lru;
    std::unordered_map<ChunkCoord, std::list<ChunkCoord>::iterator, ChunkCoordHash> m_entries;
};

} // namespace Rigel::Voxel
