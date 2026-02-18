#include "Rigel/Voxel/VoxelLod/PersistenceSource.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace Rigel::Voxel {
namespace {

size_t brickIndex(int x, int y, int z, const glm::ivec3& dims) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dims.x)
        + static_cast<size_t>(z) * static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y);
}

size_t chunkIndex(int x, int y, int z) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(Chunk::SIZE)
        + static_cast<size_t>(z) * static_cast<size_t>(Chunk::SIZE) * static_cast<size_t>(Chunk::SIZE);
}

size_t spanIndex(const Persistence::ChunkSpan& span, int x, int y, int z) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(z) * static_cast<size_t>(span.sizeX)
        + static_cast<size_t>(y) * static_cast<size_t>(span.sizeX) * static_cast<size_t>(span.sizeZ);
}

bool validSpanBounds(const Persistence::ChunkSpan& span) {
    if (span.sizeX <= 0 || span.sizeY <= 0 || span.sizeZ <= 0) {
        return false;
    }
    if (span.offsetX < 0 || span.offsetY < 0 || span.offsetZ < 0) {
        return false;
    }
    if (span.offsetX + span.sizeX > Chunk::SIZE ||
        span.offsetY + span.sizeY > Chunk::SIZE ||
        span.offsetZ + span.sizeZ > Chunk::SIZE) {
        return false;
    }
    return true;
}

} // namespace

PersistenceSource::PersistenceSource(Persistence::PersistenceService* service,
                                     Persistence::PersistenceContext context,
                                     std::string zoneId)
    : m_service(service)
    , m_context(std::move(context))
    , m_zoneId(std::move(zoneId)) {
}

void PersistenceSource::setCacheLimits(size_t maxCachedRegions, size_t maxCachedChunks) {
    m_maxCachedRegions = std::max<size_t>(1, maxCachedRegions);
    m_maxCachedChunks = std::max<size_t>(1, maxCachedChunks);
}

void PersistenceSource::invalidateChunk(ChunkCoord coord) const {
    if (!m_service || !m_context.storage) {
        return;
    }

    try {
        auto format = m_service->openFormat(m_context);
        Persistence::RegionLayout& layout = format->regionLayout();
        const Persistence::RegionKey regionKey = layout.regionForChunk(m_zoneId, coord);
        const std::string regionKeyString = regionCacheKey(regionKey);

        std::scoped_lock lock(m_cacheMutex);
        m_chunkCache.erase(coord);
        m_regionCache.erase(regionKeyString);
    } catch (const std::exception&) {
        std::scoped_lock lock(m_cacheMutex);
        m_chunkCache.erase(coord);
    }
}

BrickSampleStatus PersistenceSource::sampleBrick(const BrickSampleDesc& desc,
                                                 std::span<VoxelId> out,
                                                 const std::atomic_bool* cancel) const {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return BrickSampleStatus::Cancelled;
    }
    if (!desc.isValid()) {
        return BrickSampleStatus::Miss;
    }
    const glm::ivec3 dims = desc.outDims();
    const size_t expected = desc.outVoxelCount();
    if (expected == 0 || out.size() != expected) {
        return BrickSampleStatus::Miss;
    }

    const glm::ivec3 maxWorld = desc.worldMinVoxel +
        (dims - glm::ivec3(1)) * desc.stepVoxels;

    ChunkCoord minChunk = worldToChunk(desc.worldMinVoxel.x,
                                       desc.worldMinVoxel.y,
                                       desc.worldMinVoxel.z);
    ChunkCoord maxChunk = worldToChunk(maxWorld.x, maxWorld.y, maxWorld.z);

    const int cx0 = std::min(minChunk.x, maxChunk.x);
    const int cy0 = std::min(minChunk.y, maxChunk.y);
    const int cz0 = std::min(minChunk.z, maxChunk.z);
    const int cx1 = std::max(minChunk.x, maxChunk.x);
    const int cy1 = std::max(minChunk.y, maxChunk.y);
    const int cz1 = std::max(minChunk.z, maxChunk.z);

    std::unordered_map<ChunkCoord,
                       std::shared_ptr<std::array<BlockState, Chunk::VOLUME>>,
                       ChunkCoordHash> chunks;
    chunks.reserve(static_cast<size_t>(cx1 - cx0 + 1) *
                   static_cast<size_t>(cy1 - cy0 + 1) *
                   static_cast<size_t>(cz1 - cz0 + 1));

    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    return BrickSampleStatus::Cancelled;
                }
                ChunkCoord coord{cx, cy, cz};
                std::array<BlockState, Chunk::VOLUME> loadedBlocks{};
                if (!tryLoadChunk(coord, loadedBlocks, cancel)) {
                    return BrickSampleStatus::Miss;
                }
                chunks[coord] = std::make_shared<std::array<BlockState, Chunk::VOLUME>>(
                    std::move(loadedBlocks));
            }
        }
    }

    for (int z = 0; z < dims.z; ++z) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            return BrickSampleStatus::Cancelled;
        }
        const int wz = desc.worldMinVoxel.z + z * desc.stepVoxels;
        for (int y = 0; y < dims.y; ++y) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return BrickSampleStatus::Cancelled;
            }
            const int wy = desc.worldMinVoxel.y + y * desc.stepVoxels;
            for (int x = 0; x < dims.x; ++x) {
                const int wx = desc.worldMinVoxel.x + x * desc.stepVoxels;
                const ChunkCoord coord = worldToChunk(wx, wy, wz);
                auto it = chunks.find(coord);
                if (it == chunks.end() || !it->second) {
                    return BrickSampleStatus::Miss;
                }
                int lx = 0;
                int ly = 0;
                int lz = 0;
                worldToLocal(wx, wy, wz, lx, ly, lz);
                out[brickIndex(x, y, z, dims)] = toVoxelId((*it->second)[chunkIndex(lx, ly, lz)].id);
            }
        }
    }

    return BrickSampleStatus::Hit;
}

std::string PersistenceSource::regionCacheKey(const Persistence::RegionKey& key) {
    return key.zoneId + "|" + std::to_string(key.x) + "|" + std::to_string(key.y) + "|" +
        std::to_string(key.z);
}

bool PersistenceSource::tryLoadChunk(ChunkCoord coord,
                                     std::array<BlockState, Chunk::VOLUME>& out,
                                     const std::atomic_bool* cancel) const {
    if (!m_service || !m_context.storage) {
        return false;
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return false;
    }

    {
        std::scoped_lock lock(m_cacheMutex);
        auto chunkIt = m_chunkCache.find(coord);
        if (chunkIt != m_chunkCache.end()) {
            chunkIt->second.lastAccess = ++m_accessClock;
            if (!chunkIt->second.hit || !chunkIt->second.blocks) {
                return false;
            }
            out = *chunkIt->second.blocks;
            return true;
        }
    }

    auto format = m_service->openFormat(m_context);
    Persistence::RegionLayout& layout = format->regionLayout();
    const Persistence::RegionKey regionKey = layout.regionForChunk(m_zoneId, coord);
    const std::vector<Persistence::ChunkKey> storageKeys = layout.storageKeysForChunk(m_zoneId, coord);
    if (storageKeys.empty()) {
        std::scoped_lock lock(m_cacheMutex);
        CachedChunk miss;
        miss.hit = false;
        miss.lastAccess = ++m_accessClock;
        m_chunkCache[coord] = std::move(miss);
        evictCachesLocked();
        return false;
    }

    std::shared_ptr<Persistence::ChunkRegionSnapshot> region;
    const std::string regionKeyString = regionCacheKey(regionKey);
    {
        std::scoped_lock lock(m_cacheMutex);
        auto regionIt = m_regionCache.find(regionKeyString);
        if (regionIt != m_regionCache.end()) {
            regionIt->second.lastAccess = ++m_accessClock;
            region = regionIt->second.region;
        }
    }

    if (!region) {
        Persistence::ChunkRegionSnapshot loaded;
        try {
            loaded = m_service->loadRegion(regionKey, m_context);
        } catch (const std::exception&) {
            loaded.key = regionKey;
        }
        region = std::make_shared<Persistence::ChunkRegionSnapshot>(std::move(loaded));

        std::scoped_lock lock(m_cacheMutex);
        CachedRegion cached;
        cached.region = region;
        cached.lastAccess = ++m_accessClock;
        m_regionCache[regionKeyString] = std::move(cached);
        evictCachesLocked();
    }

    std::array<BlockState, Chunk::VOLUME> decoded{};
    const bool loaded = decodeChunkFromRegion(*region, storageKeys, coord, decoded);

    std::scoped_lock lock(m_cacheMutex);
    CachedChunk cached;
    cached.hit = loaded;
    cached.lastAccess = ++m_accessClock;
    if (loaded) {
        cached.blocks = std::make_shared<std::array<BlockState, Chunk::VOLUME>>(decoded);
    }
    m_chunkCache[coord] = std::move(cached);
    evictCachesLocked();

    if (!loaded) {
        return false;
    }
    out = decoded;
    return true;
}

bool PersistenceSource::decodeChunkFromRegion(const Persistence::ChunkRegionSnapshot& region,
                                              const std::vector<Persistence::ChunkKey>& storageKeys,
                                              ChunkCoord coord,
                                              std::array<BlockState, Chunk::VOLUME>& out) const {
    out.fill(BlockState{});

    bool any = false;
    for (const Persistence::ChunkKey& storageKey : storageKeys) {
        const Persistence::ChunkSnapshot* snapshot = nullptr;
        for (const auto& candidate : region.chunks) {
            if (candidate.key == storageKey) {
                snapshot = &candidate;
                break;
            }
        }
        if (!snapshot) {
            continue;
        }

        const Persistence::ChunkSpan& span = snapshot->data.span;
        if (span.chunkX != coord.x || span.chunkY != coord.y || span.chunkZ != coord.z) {
            continue;
        }
        if (!applySpanToChunkArray(snapshot->data, out)) {
            return false;
        }
        any = true;
    }
    return any;
}

bool PersistenceSource::applySpanToChunkArray(const Persistence::ChunkData& data,
                                              std::array<BlockState, Chunk::VOLUME>& out) {
    const Persistence::ChunkSpan& span = data.span;
    if (!validSpanBounds(span)) {
        return false;
    }
    const size_t expected = static_cast<size_t>(span.sizeX) *
        static_cast<size_t>(span.sizeY) *
        static_cast<size_t>(span.sizeZ);
    if (data.blocks.size() != expected) {
        return false;
    }

    for (int z = 0; z < span.sizeZ; ++z) {
        for (int y = 0; y < span.sizeY; ++y) {
            for (int x = 0; x < span.sizeX; ++x) {
                const int localX = span.offsetX + x;
                const int localY = span.offsetY + y;
                const int localZ = span.offsetZ + z;
                out[chunkIndex(localX, localY, localZ)] = data.blocks[spanIndex(span, x, y, z)];
            }
        }
    }
    return true;
}

void PersistenceSource::evictCachesLocked() const {
    while (m_regionCache.size() > m_maxCachedRegions) {
        auto oldest = m_regionCache.end();
        for (auto it = m_regionCache.begin(); it != m_regionCache.end(); ++it) {
            if (oldest == m_regionCache.end() || it->second.lastAccess < oldest->second.lastAccess) {
                oldest = it;
            }
        }
        if (oldest == m_regionCache.end()) {
            break;
        }
        m_regionCache.erase(oldest);
    }

    while (m_chunkCache.size() > m_maxCachedChunks) {
        auto oldest = m_chunkCache.end();
        for (auto it = m_chunkCache.begin(); it != m_chunkCache.end(); ++it) {
            if (oldest == m_chunkCache.end() || it->second.lastAccess < oldest->second.lastAccess) {
                oldest = it;
            }
        }
        if (oldest == m_chunkCache.end()) {
            break;
        }
        m_chunkCache.erase(oldest);
    }
}

} // namespace Rigel::Voxel
