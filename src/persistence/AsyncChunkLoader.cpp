#include "Rigel/Persistence/AsyncChunkLoader.h"

#include "Rigel/Persistence/ChunkSpanMerge.h"
#include "Rigel/Persistence/Containers.h"
#include "Rigel/Persistence/RegionLayout.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Core/Profiler.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>

#include <spdlog/spdlog.h>

namespace Rigel::Persistence {

size_t AsyncChunkLoader::RegionKeyHash::operator()(const RegionKey& key) const {
    size_t seed = std::hash<std::string>{}(key.zoneId);
    size_t hx = std::hash<int32_t>{}(key.x);
    size_t hy = std::hash<int32_t>{}(key.y);
    size_t hz = std::hash<int32_t>{}(key.z);
    seed ^= hx + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= hy + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= hz + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

AsyncChunkLoader::AsyncChunkLoader(PersistenceService& service,
                                   PersistenceContext context,
                                   Voxel::World& world,
                                   uint32_t worldGenVersion,
                                   size_t ioThreads,
                                   size_t workerThreads,
                                   int viewDistanceChunks,
                                   std::shared_ptr<Voxel::WorldGenerator> generator)
    : m_service(&service),
      m_context(std::move(context)),
      m_world(&world),
      m_worldGenVersion(worldGenVersion),
      m_generator(std::move(generator)),
      m_ioPool(ioThreads),
      m_workerPool(workerThreads) {
    m_format = m_service->openFormat(m_context);
    int regionSpan = estimateRegionSpan();
    if (regionSpan < 1) {
        regionSpan = 1;
    }
    int radius = viewDistanceChunks / regionSpan;
    if (radius < 1) {
        radius = 1;
    }
    if (radius > 2) {
        radius = 2;
    }
    m_prefetchRadius = radius;
}

void AsyncChunkLoader::setMaxCachedRegions(size_t maxRegions) {
    m_maxCachedRegions = maxRegions;
}

void AsyncChunkLoader::setMaxInFlightRegions(size_t maxRegions) {
    m_maxInFlightRegions = maxRegions;
}

void AsyncChunkLoader::setPrefetchRadius(int radius) {
    m_prefetchRadius = radius;
}

void AsyncChunkLoader::setLoadQueueLimit(size_t maxPending) {
    m_loadQueueLimit = maxPending;
}

bool AsyncChunkLoader::request(Voxel::ChunkCoord coord) {
    if (!m_format || !m_world) {
        return false;
    }
    if (m_loadQueueLimit > 0 &&
        m_pendingChunks.find(coord) == m_pendingChunks.end() &&
        m_pendingChunks.size() >= m_loadQueueLimit) {
        return false;
    }

    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto cacheIt = m_cache.find(key);
    if (cacheIt != m_cache.end()) {
        if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
            return false;
        }
        m_pendingChunks.insert(coord);
        queuePayloadBuild(cacheIt->second, coord);
        touch(key);
        return true;
    }
    if (m_inFlight.find(key) == m_inFlight.end()) {
        if (!regionMayExist(key)) {
            return false;
        }
    }

    m_pendingChunks.insert(coord);
    m_regionPending[key].insert(coord);
    if (queueRegionLoad(key)) {
        prefetchNeighbors(key);
    }
    return true;
}

bool AsyncChunkLoader::isPending(Voxel::ChunkCoord coord) const {
    return m_pendingChunks.find(coord) != m_pendingChunks.end();
}

void AsyncChunkLoader::cancel(Voxel::ChunkCoord coord) {
    m_pendingChunks.erase(coord);
    if (!m_format) {
        return;
    }
    RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, coord);
    auto it = m_regionPending.find(key);
    if (it != m_regionPending.end()) {
        it->second.erase(coord);
        if (it->second.empty()) {
            m_regionPending.erase(it);
        }
    }
}

void AsyncChunkLoader::drainCompletions(size_t budget) {
    {
        PROFILE_SCOPE("Streaming/LoadRegionDrain");
        drainRegionCompletions();
    }
    {
        PROFILE_SCOPE("Streaming/LoadPayloadDrain");
        drainPayloadCompletions(budget);
    }
}

void AsyncChunkLoader::drainRegionCompletions() {
    RegionResult result;
    while (m_regionComplete.tryPop(result)) {
        m_inFlight.erase(result.key);
        if (result.ok) {
            RegionPresence& presence = m_regionPresence[result.key];
            presence.exists = true;
            presence.nextCheck = std::chrono::steady_clock::time_point{};
        }
        if (!result.ok) {
            spdlog::warn("Region load failed ({} {} {}), treating as empty",
                         result.key.x, result.key.y, result.key.z);
            result.entry.region = std::make_shared<ChunkRegionSnapshot>();
            result.entry.region->key = result.key;
            result.entry.present.clear();
            result.entry.spansByCoord.clear();
        }
        m_cache[result.key] = std::move(result.entry);
        touch(result.key);
        evictIfNeeded();

        auto pendingIt = m_regionPending.find(result.key);
        if (pendingIt != m_regionPending.end()) {
            auto cacheIt = m_cache.find(result.key);
            if (cacheIt != m_cache.end()) {
                for (const auto& coord : pendingIt->second) {
                    if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
                        m_pendingChunks.erase(coord);
                        continue;
                    }
                    m_pendingChunks.insert(coord);
                    queuePayloadBuild(cacheIt->second, coord);
                }
            }
            m_regionPending.erase(pendingIt);
        }
    }
}

void AsyncChunkLoader::drainPayloadCompletions(size_t budget) {
    size_t applied = 0;
    ChunkPayload payload;
    while (applied < budget && m_chunkComplete.tryPop(payload)) {
        m_payloadInFlight.erase(payload.coord);
        if (payload.cancelled || m_pendingChunks.find(payload.coord) == m_pendingChunks.end()) {
            continue;
        }
        m_pendingChunks.erase(payload.coord);
        applyPayload(payload);
        ++applied;
    }
}

bool AsyncChunkLoader::applyPayload(const ChunkPayload& payload) {
    if (!m_world) {
        return false;
    }

    Voxel::Chunk& chunk = m_world->chunkManager().getOrCreateChunk(payload.coord);
    if (chunk.isPersistDirty()) {
        return false;
    }

    chunk.copyFrom(payload.blocks.blocks, m_world->blockRegistry());
    chunk.setWorldGenVersion(payload.worldGenVersion);
    chunk.clearPersistDirty();
    chunk.clearDirty();
    chunk.setLoadedFromDisk(payload.loadedFromDisk);

    for (int i = 0; i < Voxel::DirectionCount; ++i) {
        Voxel::Direction dir = static_cast<Voxel::Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        Voxel::directionOffset(dir, dx, dy, dz);
        Voxel::ChunkCoord neighborCoord = payload.coord.offset(dx, dy, dz);
        Voxel::Chunk* neighbor = m_world->chunkManager().getChunk(neighborCoord);
        if (neighbor) {
            neighbor->markDirty();
        }
    }

    return true;
}

bool AsyncChunkLoader::queueRegionLoad(const RegionKey& key) {
    if (m_cache.find(key) != m_cache.end()) {
        return false;
    }
    if (m_inFlight.find(key) != m_inFlight.end()) {
        return false;
    }
    if (m_maxInFlightRegions > 0 && m_inFlight.size() >= m_maxInFlightRegions) {
        return false;
    }

    m_inFlight.insert(key);
    PersistenceService* servicePtr = m_service;
    PersistenceContext contextCopy = m_context;

    auto job = [this, servicePtr, contextCopy, key]() mutable {
        RegionResult result;
        result.key = key;
        try {
            auto jobFormat = servicePtr->openFormat(contextCopy);
            ChunkRegionSnapshot region = jobFormat->chunkContainer().loadRegion(key);
            RegionEntry entry;
            entry.region = std::make_shared<ChunkRegionSnapshot>(std::move(region));
            entry.present.reserve(entry.region->chunks.size());
            entry.spansByCoord.reserve(entry.region->chunks.size());
            for (const auto& snapshot : entry.region->chunks) {
                const ChunkSpan& span = snapshot.data.span;
                Voxel::ChunkCoord coord{span.chunkX, span.chunkY, span.chunkZ};
                entry.present.insert(coord);
                entry.spansByCoord[coord].push_back(&snapshot);
            }
            result.entry = std::move(entry);
            result.ok = true;
        } catch (const std::exception& e) {
            spdlog::warn("Async region load failed ({} {} {}): {}",
                         key.x, key.y, key.z, e.what());
            result.ok = false;
        }
        m_regionComplete.push(std::move(result));
    };

    if (m_ioPool.threadCount() > 0) {
        m_ioPool.enqueue(std::move(job));
    } else {
        job();
    }
    return true;
}

void AsyncChunkLoader::queuePayloadBuild(const RegionEntry& entry, Voxel::ChunkCoord coord) {
    if (!m_generator || !m_world) {
        return;
    }
    if (m_payloadInFlight.find(coord) != m_payloadInFlight.end()) {
        return;
    }
    auto spanIt = entry.spansByCoord.find(coord);
    if (spanIt == entry.spansByCoord.end()) {
        return;
    }

    m_payloadInFlight.insert(coord);
    auto generator = m_generator;
    auto registry = &m_world->blockRegistry();
    std::vector<const ChunkSnapshot*> spans = spanIt->second;

    auto job = [this, coord, spans = std::move(spans), generator, registry]() mutable {
        ChunkPayload payload;
        payload.coord = coord;
        payload.worldGenVersion = generator ? generator->config().world.version : 0;
        payload.loadedFromDisk = true;

        Voxel::Chunk temp(coord);
        ChunkBaseFillFn baseFill;
        if (generator) {
            baseFill = [generator, coord](Voxel::Chunk& target, const Voxel::BlockRegistry& reg) {
                Voxel::ChunkBuffer buffer;
                generator->generate(coord, buffer, nullptr);
                target.copyFrom(buffer.blocks, reg);
                target.clearPersistDirty();
            };
        }
        auto mergeResult = mergeChunkSpans(temp, *registry, spans, baseFill);
        temp.copyBlocks(payload.blocks.blocks);
        payload.empty = temp.isEmpty();
        payload.cancelled = false;
        payload.loadedFromDisk = mergeResult.loadedFromDisk;
        m_chunkComplete.push(std::move(payload));
    };

    if (m_workerPool.threadCount() > 0) {
        m_workerPool.enqueue(std::move(job));
    } else {
        job();
    }
}

void AsyncChunkLoader::prefetchNeighbors(const RegionKey& center) {
    if (m_prefetchRadius <= 0) {
        return;
    }
    for (int dz = -m_prefetchRadius; dz <= m_prefetchRadius; ++dz) {
        for (int dy = -m_prefetchRadius; dy <= m_prefetchRadius; ++dy) {
            for (int dx = -m_prefetchRadius; dx <= m_prefetchRadius; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                RegionKey neighbor = center;
                neighbor.x += dx;
                neighbor.y += dy;
                neighbor.z += dz;
                queueRegionLoad(neighbor);
            }
        }
    }
}

void AsyncChunkLoader::touch(const RegionKey& key) {
    auto it = std::find(m_lru.begin(), m_lru.end(), key);
    if (it != m_lru.end()) {
        m_lru.erase(it);
    }
    m_lru.push_back(key);
}

void AsyncChunkLoader::evictIfNeeded() {
    if (m_maxCachedRegions == 0) {
        return;
    }
    while (m_cache.size() > m_maxCachedRegions && !m_lru.empty()) {
        RegionKey key = m_lru.front();
        m_lru.pop_front();
        m_cache.erase(key);
    }
}

int AsyncChunkLoader::estimateRegionSpan() const {
    if (!m_format) {
        return 1;
    }
    Voxel::ChunkCoord origin{0, 0, 0};
    RegionKey base = m_format->regionLayout().regionForChunk(m_zoneId, origin);
    constexpr int kMaxSpan = 64;
    for (int offset = 1; offset <= kMaxSpan; ++offset) {
        Voxel::ChunkCoord probe{offset, 0, 0};
        RegionKey key = m_format->regionLayout().regionForChunk(m_zoneId, probe);
        if (!(key == base)) {
            return offset;
        }
    }
    return kMaxSpan;
}

bool AsyncChunkLoader::regionMayExist(const RegionKey& key) {
    if (!m_format) {
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    auto presenceIt = m_regionPresence.find(key);
    if (presenceIt != m_regionPresence.end()) {
        if (presenceIt->second.exists) {
            return true;
        }
        if (now < presenceIt->second.nextCheck) {
            return false;
        }
    }

    bool exists = m_format->chunkContainer().regionExists(key);
    RegionPresence& presence = m_regionPresence[key];
    presence.exists = exists;
    if (exists) {
        presence.nextCheck = std::chrono::steady_clock::time_point{};
    } else {
        presence.nextCheck = now + std::chrono::seconds(2);
    }
    return exists;
}

} // namespace Rigel::Persistence
