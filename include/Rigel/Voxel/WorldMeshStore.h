#pragma once

#include "ChunkCoord.h"
#include "ChunkMesh.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace Rigel::Voxel {

struct MeshId {
    uint32_t storeId = 0;
    ChunkCoord coord{};

    bool operator==(const MeshId& other) const {
        return storeId == other.storeId && coord == other.coord;
    }
};

struct MeshIdHash {
    size_t operator()(const MeshId& id) const noexcept {
        size_t coordHash = ChunkCoordHash{}(id.coord);
        size_t storeHash = std::hash<uint32_t>{}(id.storeId);
        return coordHash ^ (storeHash + 0x9e3779b97f4a7c15ULL + (coordHash << 6) + (coordHash >> 2));
    }
};

struct MeshRevision {
    uint32_t value = 0;
};

struct WorldMeshEntry {
    ChunkCoord coord;
    ChunkMesh mesh;
    MeshId id;
    MeshRevision revision;
};

class WorldMeshStore {
public:
    WorldMeshStore()
        : m_storeId(s_nextStoreId.fetch_add(1, std::memory_order_relaxed))
    {}

    WorldMeshStore(const WorldMeshStore&) = delete;
    WorldMeshStore& operator=(const WorldMeshStore&) = delete;

    void set(ChunkCoord coord, ChunkMesh mesh) {
        std::unique_lock lock(m_mutex);
        auto [it, inserted] = m_meshes.emplace(coord, WorldMeshEntry{});
        WorldMeshEntry& entry = it->second;
        entry.coord = coord;
        if (inserted) {
            entry.id = makeMeshId(coord);
        }
        uint32_t& counter = m_revisionCounters[coord];
        uint32_t next = counter + 1;
        counter = (next == 0) ? 1 : next;
        entry.revision.value = counter;
        entry.mesh = std::move(mesh);
        m_version.fetch_add(1, std::memory_order_relaxed);
    }

    void remove(ChunkCoord coord) {
        std::unique_lock lock(m_mutex);
        bool removed = m_meshes.erase(coord) > 0;
        m_revisionCounters.erase(coord);
        if (removed) {
            m_version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void clear() {
        std::unique_lock lock(m_mutex);
        if (!m_meshes.empty()) {
            m_meshes.clear();
            m_revisionCounters.clear();
            m_version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool contains(ChunkCoord coord) const {
        std::shared_lock lock(m_mutex);
        return m_meshes.find(coord) != m_meshes.end();
    }

    void forEach(const std::function<void(const WorldMeshEntry&)>& fn) const {
        std::shared_lock lock(m_mutex);
        for (const auto& [coord, entry] : m_meshes) {
            fn(entry);
        }
    }

    uint64_t version() const { return m_version.load(std::memory_order_relaxed); }
    uint32_t storeId() const { return m_storeId; }

private:
    MeshId makeMeshId(ChunkCoord coord) const {
        return MeshId{m_storeId, coord};
    }

    static inline std::atomic<uint32_t> s_nextStoreId{1};

    uint32_t m_storeId = 0;
    mutable std::shared_mutex m_mutex;
    std::unordered_map<ChunkCoord, WorldMeshEntry, ChunkCoordHash> m_meshes;
    std::unordered_map<ChunkCoord, uint32_t, ChunkCoordHash> m_revisionCounters;
    std::atomic<uint64_t> m_version{0};
};

} // namespace Rigel::Voxel
