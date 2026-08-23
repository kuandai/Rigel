#pragma once

#include "ChunkCoord.h"
#include "ChunkMesh.h"
#include "ChunkVisibilityTrace.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

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
    uint64_t value = 0;
};

struct WorldMeshEntry {
    ChunkCoord coord;
    ChunkMesh mesh;
    MeshId id;
    MeshRevision revision;
    std::optional<ChunkVisibilityTraceLink> visibilityTrace;
};

enum class CachedMeshTraceAttachment : uint8_t {
    Missing,
    EmptyGeometry,
    NonemptyGeometry
};

class WorldMeshStore {
public:
    WorldMeshStore()
        : m_storeId(s_nextStoreId.fetch_add(1, std::memory_order_relaxed))
    {}

    ~WorldMeshStore() { clear(); }

    WorldMeshStore(const WorldMeshStore&) = delete;
    WorldMeshStore& operator=(const WorldMeshStore&) = delete;

    void set(
        ChunkCoord coord,
        ChunkMesh mesh,
        std::optional<ChunkVisibilityTraceLink> visibilityTrace = std::nullopt) {
        std::optional<ChunkVisibilityTraceLink> displacedTrace;
        {
            std::unique_lock lock(m_mutex);
            auto [it, inserted] = m_meshes.emplace(coord, WorldMeshEntry{});
            WorldMeshEntry& entry = it->second;
            entry.coord = coord;
            if (inserted) {
                entry.id = makeMeshId(coord);
            } else if (entry.visibilityTrace &&
                       (!visibilityTrace ||
                        entry.visibilityTrace->key != visibilityTrace->key)) {
                displacedTrace = std::move(entry.visibilityTrace);
            }
            entry.mesh = std::move(mesh);
            entry.visibilityTrace = entry.mesh.isEmpty()
                ? std::nullopt
                : std::move(visibilityTrace);
            entry.revision.value =
                m_version.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        finishTrace(
            displacedTrace,
            ChunkVisibilityDrawOutcome::MeshReplacedBeforeDraw);
    }

    void remove(ChunkCoord coord) {
        std::optional<ChunkVisibilityTraceLink> removedTrace;
        bool removed = false;
        {
            std::unique_lock lock(m_mutex);
            auto it = m_meshes.find(coord);
            if (it != m_meshes.end()) {
                removedTrace = std::move(it->second.visibilityTrace);
                m_meshes.erase(it);
                removed = true;
            }
            if (removed) {
                m_version.fetch_add(1, std::memory_order_relaxed);
            }
        }
        finishTrace(
            removedTrace,
            ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
    }

    void clear() {
        std::vector<ChunkVisibilityTraceLink> removedTraces;
        {
            std::unique_lock lock(m_mutex);
            removedTraces.reserve(m_meshes.size());
            for (auto& [coord, entry] : m_meshes) {
                if (entry.visibilityTrace) {
                    removedTraces.push_back(
                        std::move(*entry.visibilityTrace));
                }
            }
            if (!m_meshes.empty()) {
                m_version.fetch_add(1, std::memory_order_relaxed);
            }
            m_meshes.clear();
        }
        for (const auto& trace : removedTraces) {
            finishTrace(
                trace,
                ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
        }
    }

    CachedMeshTraceAttachment attachCachedVisibilityTrace(
        ChunkCoord coord,
        ChunkVisibilityTraceLink visibilityTrace) {
        std::optional<ChunkVisibilityTraceLink> displacedTrace;
        CachedMeshTraceAttachment result =
            CachedMeshTraceAttachment::Missing;
        {
            std::unique_lock lock(m_mutex);
            auto it = m_meshes.find(coord);
            if (it == m_meshes.end()) {
                return result;
            }
            if (it->second.mesh.isEmpty()) {
                return CachedMeshTraceAttachment::EmptyGeometry;
            }
            if (it->second.visibilityTrace &&
                it->second.visibilityTrace->key != visibilityTrace.key) {
                displacedTrace = std::move(it->second.visibilityTrace);
            }
            it->second.visibilityTrace = std::move(visibilityTrace);
            result = CachedMeshTraceAttachment::NonemptyGeometry;
        }
        finishTrace(
            displacedTrace,
            ChunkVisibilityDrawOutcome::TraceReplacedBeforeDraw);
        return result;
    }

    void endCameraVisibilityTrace(ChunkCoord coord) {
        std::optional<ChunkVisibilityTraceLink> endedTrace;
        {
            std::unique_lock lock(m_mutex);
            auto it = m_meshes.find(coord);
            if (it == m_meshes.end() || !it->second.visibilityTrace ||
                it->second.visibilityTrace->kind !=
                    ChunkVisibilityLifecycleKind::CameraDemand) {
                return;
            }
            endedTrace = std::move(it->second.visibilityTrace);
            it->second.visibilityTrace.reset();
        }
        finishTrace(
            endedTrace,
            ChunkVisibilityDrawOutcome::CameraLeftBeforeDraw);
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
    static void finishTrace(
        const std::optional<ChunkVisibilityTraceLink>& trace,
        ChunkVisibilityDrawOutcome outcome) {
        if (trace && trace->tracer) {
            trace->tracer->observeMeshUnavailable(trace->key, outcome);
        }
    }

    MeshId makeMeshId(ChunkCoord coord) const {
        return MeshId{m_storeId, coord};
    }

    static inline std::atomic<uint32_t> s_nextStoreId{1};

    uint32_t m_storeId = 0;
    mutable std::shared_mutex m_mutex;
    std::unordered_map<ChunkCoord, WorldMeshEntry, ChunkCoordHash> m_meshes;
    std::atomic<uint64_t> m_version{0};
};

} // namespace Rigel::Voxel
