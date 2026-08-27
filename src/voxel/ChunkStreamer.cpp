#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/MeshBuilder.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Preferences/UserPreferences.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <chrono>
#include <limits>
#include <sstream>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace Rigel::Voxel {

namespace {
constexpr uint64_t kEvictionRetryDelayUpdates = 60;
constexpr size_t kWorldBoundsReconciliationBudget = 64;
using FailureMap = std::map<ChunkCoord, std::string>;

void advanceFailureVersion(uint64_t& version) {
    ++version;
    if (version == 0) {
        ++version;
    }
}

void setFailure(FailureMap& failures,
                uint64_t& version,
                ChunkCoord coord,
                std::string diagnostic) {
    auto failureIt = failures.find(coord);
    if (failureIt == failures.end()) {
        failures.emplace(coord, std::move(diagnostic));
        advanceFailureVersion(version);
    } else if (failureIt->second != diagnostic) {
        failureIt->second = std::move(diagnostic);
        advanceFailureVersion(version);
    }
}

void eraseFailure(FailureMap& failures,
                  uint64_t& version,
                  ChunkCoord coord) {
    if (failures.erase(coord) > 0) {
        advanceFailureVersion(version);
    }
}

void clearFailures(FailureMap& failures, uint64_t& version) {
    if (!failures.empty()) {
        failures.clear();
        advanceFailureVersion(version);
    }
}

int distanceSquared(const ChunkCoord& a, const ChunkCoord& b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    int dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

std::string failureDiagnostic(std::string_view operation,
                              ChunkCoord coord,
                              const std::string& error) {
    std::ostringstream message;
    message << "Chunk " << operation << " failed at (" << coord.x << ", "
            << coord.y << ", " << coord.z << ')';
    if (!error.empty()) {
        message << ": " << error;
    }
    return message.str();
}

std::string diagnosticForLowestCoordinate(
    const FailureMap& errors) {
    return errors.empty() ? std::string{} : errors.begin()->second;
}

size_t visibilityKindIndex(ChunkVisibilityLifecycleKind kind) {
    return kind == ChunkVisibilityLifecycleKind::CameraDemand ? 0 : 1;
}

bool intersectsWorldBounds(
    ChunkCoord coord,
    const GeneratorDefinitionData::Bounds& world) {
    const int64_t chunkMinY =
        static_cast<int64_t>(coord.y) * Chunk::SIZE;
    const int64_t chunkMaxY = chunkMinY + Chunk::SIZE - 1;
    return chunkMaxY >= world.minY && chunkMinY <= world.maxY;
}

std::pair<int, int> visibleLocalYRange(
    ChunkCoord coord,
    const GeneratorDefinitionData::Bounds& world) {
    const int64_t chunkMinY =
        static_cast<int64_t>(coord.y) * Chunk::SIZE;
    const int first = static_cast<int>(std::clamp<int64_t>(
        static_cast<int64_t>(world.minY) - chunkMinY,
        0,
        Chunk::SIZE));
    const int onePastLast = static_cast<int>(std::clamp<int64_t>(
        static_cast<int64_t>(world.maxY) - chunkMinY + 1,
        0,
        Chunk::SIZE));
    return {first, onePastLast};
}
} // namespace

ChunkStreamer::ChunkImportance ChunkStreamer::chunkImportance(
    ChunkCoord camera,
    ChunkCoord coord) {
    const auto magnitude = [](int64_t value) {
        return static_cast<uint64_t>(value < 0 ? -value : value);
    };
    const auto saturatedAdd = [](uint64_t lhs, uint64_t rhs) {
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        return rhs > maximum - lhs ? maximum : lhs + rhs;
    };

    const uint64_t dx = magnitude(
        static_cast<int64_t>(coord.x) - camera.x);
    const uint64_t dy = magnitude(
        static_cast<int64_t>(coord.y) - camera.y);
    const uint64_t dz = magnitude(
        static_cast<int64_t>(coord.z) - camera.z);
    const uint64_t xy = saturatedAdd(dx * dx, dy * dy);
    return ChunkImportance{
        .cameraContaining = coord == camera,
        .distanceSquared = saturatedAdd(xy, dz * dz),
        .coord = coord
    };
}

ChunkStreamer::ChunkStreamer(ChunkManager& manager,
                             WorldMeshStore& meshStore,
                             BlockRegistry& registry,
                             TextureAtlas* atlas,
                             std::shared_ptr<const WorldGenerator> generator)
    : m_chunkManager(&manager),
      m_meshStore(&meshStore),
      m_registry(&registry),
      m_atlas(atlas),
      m_generator(std::move(generator)) {}

ChunkStreamer::~ChunkStreamer() {
    abandonVisibilityTraces(ChunkVisibilityOutcome::StreamerDestroyed);
    m_desiredSet.clear();
    retireAllGenerations();
    if (m_genPool) {
        m_genPool->stop();
        m_genPool.reset();
    }
    applyGenCompletions(std::numeric_limits<size_t>::max());
    if (m_meshPool) {
        m_meshPool->stop();
        m_meshPool.reset();
    }
}

void ChunkStreamer::setConfig(const StreamingConfig& config) {
    bool activeStream = m_initialStreamingBegun && m_lastCenter.has_value();
    const int previousViewDistance = std::max(0, m_config.viewDistanceChunks);
    const int previousUnloadDistance = std::max(
        previousViewDistance, m_config.unloadDistanceChunks);
    m_config = config;
    if (m_viewDistancePolicy) {
        m_config.viewDistanceChunks =
            m_viewDistancePolicy->desiredRadiusChunks();
        m_config.unloadDistanceChunks =
            m_viewDistancePolicy->unloadRadiusChunks();
    }
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    if (activeStream) {
        int viewDistance = std::max(0, m_config.viewDistanceChunks);
        int unloadDistance = std::max(
            viewDistance, m_config.unloadDistanceChunks);
        if (m_generator &&
            (viewDistance != previousViewDistance ||
             unloadDistance != previousUnloadDistance)) {
            const bool reconciliationInProgress =
                m_worldBoundsReconciliation &&
                m_worldBoundsReconciliation->deferredCursor.has_value();
            if (!m_worldBoundsReconciliation) {
                m_worldBoundsReconciliation =
                    PendingWorldBoundsReconciliation{
                        .replacement = m_generator->definition().bounds,
                        .replacementSemanticsVersion =
                            m_generator->semanticsVersion()
                    };
            }
            m_worldBoundsReconciliation->retentionCenter = *m_lastCenter;
            m_worldBoundsReconciliation->retentionRadiusSquared =
                unloadDistance * unloadDistance;
            m_worldBoundsReconciliation->revisitFromStart =
                m_worldBoundsReconciliation->revisitFromStart ||
                reconciliationInProgress;
        }
        if (viewDistance < previousViewDistance ||
            unloadDistance < previousUnloadDistance) {
            const int viewRadiusSq = viewDistance * viewDistance;
            const int unloadRadiusSq = unloadDistance * unloadDistance;
            uint64_t schedulerCoordinatesInspected = 0;
            auto remainsDirectlyDesired = [&](ChunkCoord coord) {
                return hasDirectStreamingDemand(coord);
            };
            auto remainsMeshEligible = [&](ChunkCoord coord) {
                if (!chunkIntersectsWorldBounds(coord)) {
                    return false;
                }
                const int distSq = distanceSquared(*m_lastCenter, coord);
                if (distSq <= viewRadiusSq) {
                    return true;
                }
                return distSq <= unloadRadiusSq &&
                    m_chunkManager && m_chunkManager->getChunk(coord) &&
                    m_meshStore && m_meshStore->contains(coord);
            };
            std::vector<ChunkCoord> retiredLoads;
            retiredLoads.reserve(m_loadPending.size());
            for (const auto& [coord, requestId] : m_loadPending) {
                ++schedulerCoordinatesInspected;
                if (!remainsDirectlyDesired(coord)) {
                    retiredLoads.push_back(coord);
                }
            }
            for (const ChunkCoord& coord : retiredLoads) {
                rememberConfigRetiredWork(
                    coord, ConfigRetiredWorkKind::LoadGen);
                cancelPendingLoad(coord);
            }
            std::vector<ChunkCoord> retiredGenerationFlights;
            retiredGenerationFlights.reserve(m_generationFlights.size());
            for (const auto& [coord, flight] : m_generationFlights) {
                (void)flight;
                ++schedulerCoordinatesInspected;
                if (!remainsDirectlyDesired(coord)) {
                    retiredGenerationFlights.push_back(coord);
                }
            }
            for (const ChunkCoord& coord : retiredGenerationFlights) {
                rememberConfigRetiredWork(
                    coord, ConfigRetiredWorkKind::LoadGen);
                retireGeneration(coord);
            }
            std::vector<ChunkCoord> retiredGenerations;
            retiredGenerations.reserve(m_pendingGenerations.size());
            for (const auto& [coord, importance] : m_pendingGenerations) {
                (void)importance;
                ++schedulerCoordinatesInspected;
                if (!remainsDirectlyDesired(coord)) {
                    retiredGenerations.push_back(coord);
                }
            }
            for (const ChunkCoord& coord : retiredGenerations) {
                rememberConfigRetiredWork(
                    coord, ConfigRetiredWorkKind::LoadGen);
                m_loadGenQueued.erase(coord);
                erasePendingGeneration(coord);
            }
            for (auto it = m_loadGenQueued.begin();
                 it != m_loadGenQueued.end();) {
                ++schedulerCoordinatesInspected;
                if (!remainsDirectlyDesired(*it)) {
                    rememberConfigRetiredWork(
                        *it, ConfigRetiredWorkKind::LoadGen);
                    it = m_loadGenQueued.erase(it);
                } else {
                    ++it;
                }
            }
            std::vector<std::pair<ChunkCoord, MeshRequestKind>>
                retiredMeshOwners;
            retiredMeshOwners.reserve(
                m_pendingMeshes.size() + m_meshDependencyWaiting.size());
            for (const auto& [coord, request] : m_pendingMeshes) {
                ++schedulerCoordinatesInspected;
                if (!remainsMeshEligible(coord)) {
                    retiredMeshOwners.emplace_back(coord, request.kind);
                }
            }
            for (const ChunkCoord& coord : m_meshDependencyWaiting) {
                ++schedulerCoordinatesInspected;
                if (!remainsMeshEligible(coord)) {
                    const bool hasMesh =
                        m_meshStore && m_meshStore->contains(coord);
                    retiredMeshOwners.emplace_back(
                        coord,
                        hasMesh ? MeshRequestKind::Dirty
                                : MeshRequestKind::Missing);
                }
            }
            for (const auto& [coord, kind] : retiredMeshOwners) {
                rememberConfigRetiredWork(
                    coord,
                    kind == MeshRequestKind::Dirty
                        ? ConfigRetiredWorkKind::DirtyMesh
                        : ConfigRetiredWorkKind::MissingMesh);
                retirePendingMesh(coord);
                m_priorityMeshRequests.erase(coord);
            }
            for (auto& [coord, flight] : m_meshInFlight) {
                ++schedulerCoordinatesInspected;
                if (!remainsMeshEligible(coord)) {
                    rememberConfigRetiredWork(
                        coord,
                        flight.kind == MeshRequestKind::Dirty
                            ? ConfigRetiredWorkKind::DirtyMesh
                            : ConfigRetiredWorkKind::MissingMesh);
                    flight.obsolete = true;
                    setReplacementPending(coord, flight, false);
                    flight.prioritized = false;
                    m_priorityMeshRequests.erase(coord);
                }
            }
            for (auto it = m_priorityMeshRequests.begin();
                 it != m_priorityMeshRequests.end();) {
                ++schedulerCoordinatesInspected;
                if (!remainsMeshEligible(*it)) {
                    it = m_priorityMeshRequests.erase(it);
                } else {
                    ++it;
                }
            }
            m_workMetrics.schedulerCoordinatesInspected +=
                schedulerCoordinatesInspected;
        }
        m_desiredSetRebuildPending = true;
        if (viewDistance >= previousViewDistance &&
            unloadDistance >= previousUnloadDistance) {
            m_workMetrics.residentEvictionCoordinatesInspected +=
                reconcileDeferredWorldBounds();
        }
        m_lastViewDistance = -1;
        m_lastUnloadDistance = -1;
        ensureThreadPool();
        refreshDiagnostics(false);
        return;
    }

    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::Reset);
            break;
        }
    }

    m_desired.clear();
    m_desiredSet.clear();
    m_desiredPriority.clear();
    m_pendingMeshQueues = {};
    m_pendingMeshes.clear();
    m_priorityMeshRequests.clear();
    m_evictionRetryAfter.clear();
    m_versionReplacementRetries.clear();
    m_versionReplacementWaiting.clear();
    clearFailures(m_generationErrors, m_generationFailureVersion);
    clearFailures(m_loadErrors, m_loadFailureVersion);
    clearFailures(m_meshErrors, m_meshFailureVersion);
    clearFailures(m_evictionErrors, m_evictionFailureVersion);
    m_nextEvictionRetrySequence = 0;
    m_evictionRetryScanCursor.reset();
    m_evictionRetryScanActive = false;
    m_loadGenQueue.clear();
    m_loadGenQueued.clear();
    m_pendingGenerationQueue.clear();
    m_pendingGenerations.clear();
    m_meshDependencyWaiting.clear();
    m_configRetiredWork.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_desiredSetRebuildPending = false;
    m_worldBoundsReconciliation.reset();
    m_initialStreamingBegun = false;
    m_workObservedThisUpdate = false;
    m_workStartedThisUpdate = false;
    m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    m_nextPendingMeshSequence = 1;
    ensureThreadPool();
    refreshDiagnostics(false);
}

ChunkStreamer::ViewDistancePolicyState ChunkStreamer::applyViewDistancePolicy(
    std::shared_ptr<const ViewDistancePolicy> policy) {
    if (!policy) {
        throw std::invalid_argument(
            "ChunkStreamer requires a complete View Distance policy");
    }
    ViewDistancePolicyState previous{
        .policy = m_viewDistancePolicy,
        .viewDistanceChunks = m_config.viewDistanceChunks,
        .unloadDistanceChunks = m_config.unloadDistanceChunks,
        .lastViewDistance = m_lastViewDistance,
        .lastUnloadDistance = m_lastUnloadDistance,
        .desiredSetRebuildPending = m_desiredSetRebuildPending,
        .worldBoundsReconciliation = m_worldBoundsReconciliation,
        .diagnostics = m_diagnostics
    };
    StreamingConfig updated = m_config;
    updated.viewDistanceChunks = policy->desiredRadiusChunks();
    updated.unloadDistanceChunks = policy->unloadRadiusChunks();
    const bool activeStream =
        m_initialStreamingBegun && m_lastCenter.has_value();
    const int previousViewDistance = std::max(0, m_config.viewDistanceChunks);
    const int previousUnloadDistance = std::max(
        previousViewDistance, m_config.unloadDistanceChunks);
    m_viewDistancePolicy = std::move(policy);
    m_config.viewDistanceChunks = updated.viewDistanceChunks;
    m_config.unloadDistanceChunks = updated.unloadDistanceChunks;
    if (!activeStream) {
        refreshDiagnostics(false);
        return previous;
    }

    const int viewDistance = std::max(0, updated.viewDistanceChunks);
    const int unloadDistance = std::max(
        viewDistance, updated.unloadDistanceChunks);
    if (viewDistance == previousViewDistance &&
        unloadDistance == previousUnloadDistance) {
        return previous;
    }
    if (m_generator) {
        const bool reconciliationInProgress =
            m_worldBoundsReconciliation &&
            m_worldBoundsReconciliation->deferredCursor.has_value();
        if (!m_worldBoundsReconciliation) {
            m_worldBoundsReconciliation = PendingWorldBoundsReconciliation{
                .replacement = m_generator->definition().bounds,
                .replacementSemanticsVersion =
                    m_generator->semanticsVersion()
            };
        }
        m_worldBoundsReconciliation->retentionCenter = *m_lastCenter;
        m_worldBoundsReconciliation->retentionRadiusSquared =
            unloadDistance * unloadDistance;
        m_worldBoundsReconciliation->revisitFromStart =
            m_worldBoundsReconciliation->revisitFromStart ||
            reconciliationInProgress;
    }
    m_desiredSetRebuildPending = true;
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    refreshDiagnostics(false);
    return previous;
}

void ChunkStreamer::restoreViewDistancePolicy(
    ViewDistancePolicyState state) noexcept {
    m_viewDistancePolicy = std::move(state.policy);
    m_config.viewDistanceChunks = state.viewDistanceChunks;
    m_config.unloadDistanceChunks = state.unloadDistanceChunks;
    m_lastViewDistance = state.lastViewDistance;
    m_lastUnloadDistance = state.lastUnloadDistance;
    m_desiredSetRebuildPending = state.desiredSetRebuildPending;
    m_worldBoundsReconciliation =
        std::move(state.worldBoundsReconciliation);
    m_diagnostics = std::move(state.diagnostics);
}

void ChunkStreamer::setGenerator(std::shared_ptr<const WorldGenerator> generator) {
    if (m_generator == generator) {
        return;
    }

    std::vector<ChunkCoord> enteringWorldBounds;
    const bool boundsChanged = m_generator && generator &&
        (m_generator->definition().bounds.minY != generator->definition().bounds.minY ||
         m_generator->definition().bounds.maxY != generator->definition().bounds.maxY);
    if (m_chunkManager && generator && (boundsChanged || !m_generator)) {
        const auto& replacementWorld = generator->definition().bounds;
        const bool supersedesPendingReconciliation =
            boundsChanged && m_worldBoundsReconciliation &&
            m_worldBoundsReconciliation->remeshIntersectingRows;
        const bool reconciliationInProgress =
            m_worldBoundsReconciliation &&
            m_worldBoundsReconciliation->deferredCursor.has_value();
        if (!m_worldBoundsReconciliation) {
            m_worldBoundsReconciliation = PendingWorldBoundsReconciliation{
                .replacement = replacementWorld,
                .replacementSemanticsVersion = generator->semanticsVersion()
            };
        } else {
            m_worldBoundsReconciliation->replacement = replacementWorld;
            m_worldBoundsReconciliation->replacementSemanticsVersion =
                generator->semanticsVersion();
        }
        if (boundsChanged &&
            !m_worldBoundsReconciliation->previous.has_value()) {
            m_worldBoundsReconciliation->previous =
                m_generator->definition().bounds;
        }
        m_worldBoundsReconciliation->remeshIntersectingRows =
            m_worldBoundsReconciliation->remeshIntersectingRows ||
            boundsChanged;
        m_worldBoundsReconciliation->forceRemeshIntersecting =
            m_worldBoundsReconciliation->forceRemeshIntersecting ||
            supersedesPendingReconciliation;
        if (m_lastCenter) {
            const int unloadDistance = std::max(
                std::max(0, m_config.viewDistanceChunks),
                m_config.unloadDistanceChunks);
            m_worldBoundsReconciliation->retentionCenter = *m_lastCenter;
            m_worldBoundsReconciliation->retentionRadiusSquared =
                unloadDistance * unloadDistance;
        }
        m_worldBoundsReconciliation->revisitFromStart =
            m_worldBoundsReconciliation->revisitFromStart ||
            reconciliationInProgress;
        std::vector<ChunkCoord> reconciliationCoordinates = m_desired;
        if (m_lastCenter) {
            const int viewDistance = std::max(
                0, m_config.viewDistanceChunks);
            const int viewRadiusSq = viewDistance * viewDistance;
            const int minWorldChunkY = worldToChunk(
                0, replacementWorld.minY, 0).y;
            const int maxWorldChunkY = worldToChunk(
                0, replacementWorld.maxY, 0).y;
            const int64_t firstChunkY = std::max<int64_t>(
                static_cast<int64_t>(m_lastCenter->y) - viewDistance,
                minWorldChunkY);
            const int64_t lastChunkY = std::min<int64_t>(
                static_cast<int64_t>(m_lastCenter->y) + viewDistance,
                maxWorldChunkY);
            for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
                for (int64_t chunkY = firstChunkY;
                     chunkY <= lastChunkY; ++chunkY) {
                    for (int dx = -viewDistance;
                         dx <= viewDistance; ++dx) {
                        ChunkCoord coord{
                            m_lastCenter->x + dx,
                            static_cast<int32_t>(chunkY),
                            m_lastCenter->z + dz};
                        if (distanceSquared(*m_lastCenter, coord) <=
                            viewRadiusSq) {
                            reconciliationCoordinates.push_back(coord);
                        }
                    }
                }
            }
        }
        std::sort(
            reconciliationCoordinates.begin(),
            reconciliationCoordinates.end());
        reconciliationCoordinates.erase(
            std::unique(
                reconciliationCoordinates.begin(),
                reconciliationCoordinates.end()),
            reconciliationCoordinates.end());

        auto reconcileEnteringChunk = [&](ChunkCoord coord,
                                           Chunk& chunk,
                                           bool forceRemesh) {
            const bool replacementVersion =
                chunk.worldGenVersion() == generator->semanticsVersion();
            if (chunk.isEmpty() || !replacementVersion) {
                m_worldBoundsSuppressedMeshes.erase(coord);
                if (!chunk.isEmpty()) {
                    m_states[coord] = ChunkState::ReadyData;
                }
                return;
            }

            m_chunkManager->invalidateFaceNeighbors(coord);
            if (forceRemesh ||
                m_worldBoundsSuppressedMeshes.find(coord) !=
                    m_worldBoundsSuppressedMeshes.end()) {
                enteringWorldBounds.push_back(coord);
            }
        };

        const GeneratorDefinitionData::Bounds* previousWorld = boundsChanged
            ? &m_generator->definition().bounds
            : nullptr;
        for (const ChunkCoord& coord : reconciliationCoordinates) {
            ++m_workMetrics.schedulerCoordinatesInspected;
            Chunk* chunk = m_chunkManager->getChunk(coord);
            if (!chunk) {
                continue;
            }
            if (previousWorld) {
                const bool previouslyInside =
                    intersectsWorldBounds(coord, *previousWorld);
                const bool replacementInside =
                    intersectsWorldBounds(coord, replacementWorld);
                if (previouslyInside == replacementInside) {
                    if (replacementInside &&
                        visibleLocalYRange(coord, *previousWorld) !=
                            visibleLocalYRange(coord, replacementWorld)) {
                        reconcileEnteringChunk(coord, *chunk, true);
                    }
                    continue;
                }
                if (!replacementInside) {
                    const bool alreadySuppressed =
                        m_worldBoundsSuppressedMeshes.find(coord) !=
                            m_worldBoundsSuppressedMeshes.end();
                    if (!chunk->isEmpty() && !alreadySuppressed) {
                        m_chunkManager->invalidateFaceNeighbors(coord);
                    }
                    if (m_meshStore && m_meshStore->contains(coord)) {
                        m_meshStore->remove(coord);
                        if (!chunk->isEmpty()) {
                            m_states[coord] = ChunkState::ReadyMesh;
                            m_worldBoundsSuppressedMeshes.insert(coord);
                        }
                    }
                    if (chunk->isEmpty()) {
                        m_worldBoundsSuppressedMeshes.erase(coord);
                    }
                    continue;
                }
                reconcileEnteringChunk(coord, *chunk, false);
            } else if (
                m_worldBoundsSuppressedMeshes.find(coord) !=
                    m_worldBoundsSuppressedMeshes.end() &&
                intersectsWorldBounds(coord, replacementWorld)) {
                reconcileEnteringChunk(coord, *chunk, false);
            }
        }
    } else if (generator && m_worldBoundsReconciliation) {
        // A same-bounds generator replacement can supersede an unfinished
        // transition. Restart against the current generator so deferred work
        // always converges from installed physical state to the final bounds.
        m_worldBoundsReconciliation->replacement = generator->definition().bounds;
        m_worldBoundsReconciliation->replacementSemanticsVersion =
            generator->semanticsVersion();
        m_worldBoundsReconciliation->revisitFromStart =
            m_worldBoundsReconciliation->revisitFromStart ||
            m_worldBoundsReconciliation->deferredCursor.has_value();
    } else if (!generator) {
        m_worldBoundsReconciliation.reset();
    }

    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::GeneratorReplaced);
            break;
        }
    }

    retireAllGenerations();

    std::vector<std::pair<ChunkCoord, bool>> retiredMeshRequests;
    retiredMeshRequests.reserve(
        m_pendingMeshes.size() + m_meshDependencyWaiting.size());
    std::unordered_set<ChunkCoord, ChunkCoordHash> retiredCoordinates;
    auto rememberRetiredRequest = [&](ChunkCoord coord) {
        if (retiredCoordinates.insert(coord).second) {
            retiredMeshRequests.emplace_back(
                coord,
                m_priorityMeshRequests.find(coord) !=
                    m_priorityMeshRequests.end());
        }
    };
    for (const auto& entry : m_pendingMeshes) {
        rememberRetiredRequest(entry.first);
    }
    for (const ChunkCoord& coord : m_meshDependencyWaiting) {
        rememberRetiredRequest(coord);
    }
    m_pendingMeshQueues = {};
    m_pendingMeshes.clear();
    m_meshDependencyWaiting.clear();
    m_priorityMeshRequests.clear();
    m_countedMeshRetryRevisions.clear();
    m_nextPendingMeshSequence = 1;

    uint64_t nextEpoch = m_workEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nextEpoch == 0) {
        m_workEpoch.store(1, std::memory_order_relaxed);
    }
    m_generator = std::move(generator);
    // The cached desired set is shaped by the generator's vertical bounds.
    // Keep this explicit owner until update performs the bounded rebuild.
    m_desiredSetRebuildPending =
        m_generator != nullptr && m_lastCenter.has_value();
    if (m_desiredSetRebuildPending && m_worldBoundsReconciliation) {
        // The synchronous bounded pass still sees the old desired set. Keep a
        // final pass owned until update installs replacement demand so exterior
        // residents can be durably evicted rather than only hidden.
        m_worldBoundsReconciliation->revisitFromStart = true;
    }

    // The desired set is rebuilt on the next update, but source ownership must
    // respect replacement bounds immediately. Otherwise an old disk request
    // can install an exterior payload between setGenerator() and update().
    std::vector<ChunkCoord> retiredLoads;
    retiredLoads.reserve(m_loadPending.size());
    for (const auto& [coord, requestId] : m_loadPending) {
        (void)requestId;
        ++m_workMetrics.schedulerCoordinatesInspected;
        if (!chunkIntersectsWorldBounds(coord)) {
            retiredLoads.push_back(coord);
        }
    }
    for (const ChunkCoord& coord : retiredLoads) {
        cancelPendingLoad(coord);
    }
    for (auto it = m_loadGenQueued.begin(); it != m_loadGenQueued.end();) {
        ++m_workMetrics.schedulerCoordinatesInspected;
        if (!chunkIntersectsWorldBounds(*it)) {
            it = m_loadGenQueued.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<ChunkCoord> retiredPendingGenerations;
    retiredPendingGenerations.reserve(m_pendingGenerations.size());
    for (const auto& [coord, importance] : m_pendingGenerations) {
        (void)importance;
        ++m_workMetrics.schedulerCoordinatesInspected;
        if (!chunkIntersectsWorldBounds(coord)) {
            retiredPendingGenerations.push_back(coord);
        }
    }
    for (const ChunkCoord& coord : retiredPendingGenerations) {
        erasePendingGeneration(coord);
        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end() &&
            stateIt->second == ChunkState::QueuedGen) {
            m_states.erase(stateIt);
        }
    }
    for (const ChunkCoord& coord : enteringWorldBounds) {
        if (Chunk* chunk = m_chunkManager->getChunk(coord)) {
            chunk->invalidateMesh();
        }
        queueDirtyMesh(coord);
    }
    for (auto& [coord, flight] : m_meshInFlight) {
        const bool prioritized = flight.prioritized;
        flight.obsolete = true;
        setReplacementPending(coord, flight, false);
        completeInFlightVisibilityTrace(
            flight,
            ChunkVisibilityOutcome::GeneratorReplaced);
        Chunk* chunk = dirtyMeshPriority(coord)
            ? m_chunkManager->getChunk(coord)
            : nullptr;
        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end() &&
            stateIt->second == ChunkState::QueuedMesh) {
            if (chunk && !chunk->isEmpty()) {
                stateIt->second = ChunkState::ReadyData;
            } else {
                m_states.erase(stateIt);
            }
        }
        if (chunk && !chunk->isEmpty() && m_generator &&
            chunk->worldGenVersion() ==
                m_generator->semanticsVersion()) {
            chunk->markDirty();
            if (hasEligibleMeshWork(coord)) {
                setReplacementPending(coord, flight, true);
                flight.prioritized = prioritized;
                continue;
            }
        }
        flight.prioritized = false;
    }

    for (const ChunkCoord& coord : m_desired) {
        queueLoadGen(coord);
    }
    for (const auto& [coord, prioritized] : retiredMeshRequests) {
        if (m_desiredSet.find(coord) != m_desiredSet.end()) {
            if (prioritized && hasEligibleMeshWork(coord)) {
                m_priorityMeshRequests.insert(coord);
            }
            continue;
        }
        if (hasEligibleMeshWork(coord)) {
            queueDirtyMesh(coord, prioritized);
        } else {
            m_priorityMeshRequests.erase(coord);
        }
    }
    m_workMetrics.residentEvictionCoordinatesInspected +=
        reconcileDeferredWorldBounds();
    refreshDiagnostics(false);
}

uint64_t ChunkStreamer::reconcileDeferredWorldBounds() {
    if (!m_worldBoundsReconciliation || !m_chunkManager || !m_generator) {
        return 0;
    }

    auto& reconciliation = *m_worldBoundsReconciliation;
    auto residentIt = reconciliation.deferredCursor
        ? m_streamedResidents.upper_bound(*reconciliation.deferredCursor)
        : m_streamedResidents.begin();
    size_t inspected = 0;
    while (residentIt != m_streamedResidents.end() &&
           inspected < kWorldBoundsReconciliationBudget) {
        const ChunkCoord coord = *residentIt;
        reconciliation.deferredCursor = coord;
        ++residentIt;
        ++inspected;

        Chunk* chunk = m_chunkManager->getChunk(coord);
        if (!chunk) {
            m_streamedResidents.erase(coord);
            continue;
        }
        const bool replacementInside = intersectsWorldBounds(
            coord, reconciliation.replacement);
        const bool suppressed =
            m_worldBoundsSuppressedMeshes.find(coord) !=
            m_worldBoundsSuppressedMeshes.end();
        const bool evictionDeferred =
            m_evictionRetryAfter.find(coord) != m_evictionRetryAfter.end();
        const bool visibleRowsChanged = replacementInside &&
            reconciliation.remeshIntersectingRows &&
            (reconciliation.forceRemeshIntersecting ||
             (reconciliation.previous &&
              visibleLocalYRange(coord, *reconciliation.previous) !=
                  visibleLocalYRange(coord, reconciliation.replacement)));
        const bool outsideRetention = reconciliation.retentionCenter &&
            distanceSquared(*reconciliation.retentionCenter, coord) >
                reconciliation.retentionRadiusSquared;
        if (replacementInside && !suppressed && !evictionDeferred &&
            !visibleRowsChanged && !outsideRetention) {
            continue;
        }

        if (!replacementInside) {
            if (!chunk->isEmpty() && !suppressed) {
                m_chunkManager->invalidateFaceNeighbors(coord);
            }
            if (m_meshStore && m_meshStore->contains(coord)) {
                m_meshStore->remove(coord);
                if (!chunk->isEmpty()) {
                    m_states[coord] = ChunkState::ReadyMesh;
                    m_worldBoundsSuppressedMeshes.insert(coord);
                }
            }
            if (chunk->isEmpty()) {
                m_worldBoundsSuppressedMeshes.erase(coord);
            }
            if (m_desiredSet.find(coord) == m_desiredSet.end() &&
                evictChunk(coord)) {
                m_cache.erase(coord);
            }
            continue;
        }

        if (outsideRetention) {
            if (evictChunk(coord)) {
                m_cache.erase(coord);
            }
            continue;
        }

        const bool withinRetention = reconciliation.retentionCenter
            ? !outsideRetention
            : m_lastCenter && distanceSquared(*m_lastCenter, coord) <=
                std::max(
                    std::max(0, m_config.viewDistanceChunks),
                    m_config.unloadDistanceChunks) *
                std::max(
                    std::max(0, m_config.viewDistanceChunks),
                    m_config.unloadDistanceChunks);
        const bool cachePressure = m_cache.maxChunks() > 0 &&
            m_cache.size() > m_cache.maxChunks() &&
            m_desiredSet.find(coord) == m_desiredSet.end();
        if (withinRetention && !cachePressure &&
            m_versionReplacementRetries.find(coord) ==
                m_versionReplacementRetries.end()) {
            const bool meshWorkEligible =
                hasMeshReconciliationWork(coord);
            m_evictionRetryAfter.erase(coord);
            eraseFailure(
                m_evictionErrors, m_evictionFailureVersion, coord);
            if (m_evictionRetryAfter.empty()) {
                m_nextEvictionRetrySequence = 0;
            }
            if (meshWorkEligible) {
                queueDirtyMesh(coord);
            }
        }

        const bool replacementVersion =
            chunk->worldGenVersion() ==
                reconciliation.replacementSemanticsVersion;
        if (chunk->isEmpty() || !replacementVersion) {
            m_worldBoundsSuppressedMeshes.erase(coord);
            if (!chunk->isEmpty()) {
                m_states[coord] = ChunkState::ReadyData;
            }
            continue;
        }

        if (!visibleRowsChanged &&
            m_worldBoundsSuppressedMeshes.find(coord) ==
                m_worldBoundsSuppressedMeshes.end()) {
            continue;
        }
        m_chunkManager->invalidateFaceNeighbors(coord);
        chunk->invalidateMesh();
        queueDirtyMesh(coord);
    }

    if (residentIt == m_streamedResidents.end()) {
        if (reconciliation.revisitFromStart) {
            reconciliation.deferredCursor.reset();
            reconciliation.revisitFromStart = false;
        } else {
            m_worldBoundsReconciliation.reset();
        }
    }
    return inspected;
}

void ChunkStreamer::setBenchmark(ChunkBenchmarkStats* stats) {
    m_benchmark = stats;
}

void ChunkStreamer::setVisibilityTracer(
    std::shared_ptr<ChunkVisibilityTracer> tracer) {
    tracer = tracer && tracer->enabled() ? std::move(tracer) : nullptr;
    if (tracer == m_visibilityTracer) {
        return;
    }
    const auto previousTracer = m_visibilityTracer;
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::TracerReplaced);
            break;
        }
    }
    for (auto& [coord, flight] : m_meshInFlight) {
        if (flight.visibilityTracer == previousTracer) {
            completeInFlightVisibilityTrace(
                flight,
                ChunkVisibilityOutcome::TracerReplaced);
        }
    }
    if (m_meshStore && previousTracer) {
        m_meshStore->endVisibilityTrace(
            previousTracer->coord(),
            previousTracer,
            ChunkVisibilityDrawOutcome::TraceReplacedBeforeDraw);
    }
    m_visibilityTracer = std::move(tracer);
    if (!m_visibilityTracer || !m_chunkManager) {
        return;
    }
    const ChunkCoord traced = m_visibilityTracer->coord();
    if (m_desiredSet.find(traced) == m_desiredSet.end() ||
        !m_chunkManager->getChunk(traced)) {
        return;
    }
    for (int index = 0; index < DirectionCount; ++index) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(static_cast<Direction>(index), dx, dy, dz);
        const ChunkCoord neighbor = traced.offset(dx, dy, dz);
        if (m_desiredSet.find(neighbor) != m_desiredSet.end() &&
            !m_chunkManager->getChunk(neighbor) &&
            m_generationFlights.find(neighbor) !=
                m_generationFlights.end()) {
            ensureVisibilityTrace(
                traced,
                ChunkVisibilityLifecycleKind::CameraDemand,
                ChunkVisibilityOrigin::ResidentLeftCensored);
            markVisibilityMeshEligible(traced, false);
            return;
        }
    }
}

void ChunkStreamer::setChunkLoader(ChunkLoadCallback loader) {
    m_chunkLoader = std::move(loader);
    for (const ChunkCoord& coord : m_desired) {
        queueLoadGen(coord);
    }
}

void ChunkStreamer::setChunkPendingCallback(ChunkPendingCallback pending) {
    m_chunkPending = std::move(pending);
}

void ChunkStreamer::setChunkLoadDrain(ChunkLoadDrainCallback drain) {
    m_chunkLoadDrain = std::move(drain);
}

void ChunkStreamer::setChunkLoadCancel(ChunkLoadCancelCallback cancel) {
    m_chunkLoadCancel = std::move(cancel);
}

void ChunkStreamer::setChunkLoadDiagnosticsCallback(
    ChunkLoadDiagnosticsCallback diagnostics) {
    m_chunkLoadDiagnostics = std::move(diagnostics);
    refreshDiagnostics(false);
}

void ChunkStreamer::setChunkLoadExecutionStateCallback(
    ChunkLoadExecutionStateCallback executionState) {
    m_chunkLoadExecutionState = std::move(executionState);
}

void ChunkStreamer::setChunkEvictionCallback(ChunkEvictionCallback evict) {
    m_chunkEviction = std::move(evict);
}

void ChunkStreamer::markSpawnDiscoveryComplete() {
    m_spawnDiscoveryComplete = true;
    refreshDiagnostics(false);
}

void ChunkStreamer::prioritizeMesh(ChunkCoord coord) {
    queueDirtyMesh(coord, true);
    refreshDiagnostics(false);
}

void ChunkStreamer::update(const glm::vec3& cameraPos) {
    m_workMetrics.lastUpdateDesiredBuildCoordinatesInspected = 0;
    m_workMetrics.lastUpdateDesiredBuildCoordinatesSkippedByWorldBounds = 0;
    m_workMetrics.lastUpdateSchedulerCoordinatesInspected = 0;
    m_workMetrics.lastUpdateCacheEvictionCoordinatesInspected = 0;
    m_workMetrics.lastUpdateResidentEvictionCoordinatesInspected = 0;
    m_workMetrics.lastUpdateDeferredEvictionCoordinatesInspected = 0;
    const uint64_t schedulerCoordinatesInspectedBeforeUpdate =
        m_workMetrics.schedulerCoordinatesInspected;
    if (!m_chunkManager || !m_generator || !m_meshStore) {
        return;
    }

    m_initialStreamingBegun = true;
    ++m_streamingUpdateSequence;
    StreamingDiagnosticSnapshot beforeUpdate = collectDiagnostics();
    m_workObservedThisUpdate = !beforeUpdate.workEmpty();
    m_workStartedThisUpdate = false;
    uint64_t desiredBuildCoordinatesInspected = 0;
    uint64_t desiredBuildCoordinatesSkippedByWorldBounds = 0;
    uint64_t schedulerCoordinatesInspected = 0;
    uint64_t cacheEvictionCoordinatesInspected = 0;
    uint64_t residentEvictionCoordinatesInspected = 0;
    uint64_t deferredEvictionCoordinatesInspected = 0;

    ChunkCoord center = cameraToChunk(cameraPos);
    int viewDistance = std::max(0, m_config.viewDistanceChunks);
    int unloadDistance = std::max(viewDistance, m_config.unloadDistanceChunks);
    int viewRadiusSq = viewDistance * viewDistance;
    int unloadRadiusSq = unloadDistance * unloadDistance;

    const bool demandShapeChanged = !m_lastCenter ||
        *m_lastCenter != center ||
        m_lastViewDistance != viewDistance ||
        m_lastUnloadDistance != unloadDistance;
    const bool generatorRequestedRebuild = m_desiredSetRebuildPending;
    bool rebuildDesired = demandShapeChanged || generatorRequestedRebuild;
    bool cacheEvictionNeeded = demandShapeChanged;
    const bool boundedResidentReconciliation = demandShapeChanged &&
        (generatorRequestedRebuild || m_worldBoundsReconciliation.has_value());
    if (boundedResidentReconciliation) {
        const bool reconciliationInProgress =
            m_worldBoundsReconciliation &&
            m_worldBoundsReconciliation->deferredCursor.has_value();
        if (!m_worldBoundsReconciliation) {
            m_worldBoundsReconciliation = PendingWorldBoundsReconciliation{
                .replacement = m_generator->definition().bounds,
                .replacementSemanticsVersion =
                    m_generator->semanticsVersion()
            };
        }
        m_worldBoundsReconciliation->retentionCenter = center;
        m_worldBoundsReconciliation->retentionRadiusSquared = unloadRadiusSq;
        m_worldBoundsReconciliation->revisitFromStart =
            m_worldBoundsReconciliation->revisitFromStart ||
            reconciliationInProgress;
    }

    if (rebuildDesired) {
        PROFILE_SCOPE("Streaming/Update/DesiredBuild");
        auto previousDesired = std::move(m_desiredSet);
        auto previouslyQueued = std::move(m_loadGenQueued);
        m_loadGenQueue.clear();
        m_loadGenQueued.clear();

        const int minWorldChunkY = worldToChunk(
            0, m_generator->definition().bounds.minY, 0).y;
        const int maxWorldChunkY = worldToChunk(
            0, m_generator->definition().bounds.maxY, 0).y;
        const int64_t requestedMinChunkY =
            static_cast<int64_t>(center.y) - viewDistance;
        const int64_t requestedMaxChunkY =
            static_cast<int64_t>(center.y) + viewDistance;
        const int64_t firstChunkY = std::max<int64_t>(
            requestedMinChunkY, minWorldChunkY);
        const int64_t lastChunkY = std::min<int64_t>(
            requestedMaxChunkY, maxWorldChunkY);
        const size_t diameter = static_cast<size_t>(viewDistance * 2 + 1);
        const size_t clippedLayerCount = firstChunkY <= lastChunkY
            ? static_cast<size_t>(lastChunkY - firstChunkY + 1)
            : 0;
        desiredBuildCoordinatesSkippedByWorldBounds =
            static_cast<uint64_t>(
                diameter * diameter * (diameter - clippedLayerCount));

        std::vector<ChunkImportance> desired;
        desired.reserve(diameter * diameter * clippedLayerCount);

        for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
            for (int64_t chunkY = firstChunkY;
                 chunkY <= lastChunkY; ++chunkY) {
                for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
                    ++desiredBuildCoordinatesInspected;
                    ChunkCoord coord{
                        center.x + dx,
                        static_cast<int32_t>(chunkY),
                        center.z + dz};
                    const ChunkImportance importance =
                        chunkImportance(center, coord);
                    if (importance.distanceSquared >
                        static_cast<uint64_t>(viewRadiusSq)) {
                        continue;
                    }
                    desired.push_back(importance);
                }
            }
        }

        std::sort(
            desired.begin(), desired.end(), ChunkImportancePrecedes{});

        m_desired.clear();
        m_desiredSet.clear();
        m_desiredPriority.clear();
        m_desired.reserve(desired.size());
        m_desiredSet.reserve(desired.size());
        m_desiredPriority.reserve(desired.size());
        for (size_t priority = 0; priority < desired.size(); ++priority) {
            const auto& entry = desired[priority];
            m_desired.push_back(entry.coord);
            m_desiredSet.insert(entry.coord);
            m_desiredPriority.emplace(entry.coord, priority);
        }
        m_lastCenter = center;
        m_lastViewDistance = viewDistance;
        m_lastUnloadDistance = unloadDistance;
        reprioritizePendingGenerations(schedulerCoordinatesInspected);
        if (demandShapeChanged && !boundedResidentReconciliation) {
            deferredEvictionCoordinatesInspected = retireIneligibleEvictions(
                center, viewRadiusSq, unloadRadiusSq);
        }
        std::vector<ChunkCoord> outsideMeshRetention;
        outsideMeshRetention.reserve(
            m_pendingMeshes.size() + m_meshDependencyWaiting.size());
        for (const auto& [coord, request] : m_pendingMeshes) {
            ++schedulerCoordinatesInspected;
            if (m_desiredSet.find(coord) == m_desiredSet.end() &&
                distanceSquared(center, coord) > unloadRadiusSq) {
                outsideMeshRetention.push_back(coord);
            }
        }
        for (const ChunkCoord& coord : m_meshDependencyWaiting) {
            ++schedulerCoordinatesInspected;
            if (m_desiredSet.find(coord) == m_desiredSet.end() &&
                distanceSquared(center, coord) > unloadRadiusSq) {
                outsideMeshRetention.push_back(coord);
            }
        }
        for (const ChunkCoord& coord : outsideMeshRetention) {
            retirePendingMesh(coord);
            m_priorityMeshRequests.erase(coord);
            auto stateIt = m_states.find(coord);
            if (stateIt != m_states.end() &&
                stateIt->second == ChunkState::QueuedMesh) {
                m_states.erase(stateIt);
            }
        }
        for (auto& [coord, flight] : m_meshInFlight) {
            ++schedulerCoordinatesInspected;
            if (m_desiredSet.find(coord) == m_desiredSet.end() &&
                distanceSquared(center, coord) > unloadRadiusSq) {
                flight.obsolete = true;
                setReplacementPending(coord, flight, false);
                flight.prioritized = false;
                m_priorityMeshRequests.erase(coord);
                auto stateIt = m_states.find(coord);
                if (stateIt != m_states.end() &&
                    stateIt->second == ChunkState::QueuedMesh) {
                    m_states.erase(stateIt);
                }
            }
        }
        reprioritizePendingMeshes(schedulerCoordinatesInspected);
        for (const ChunkCoord& coord : m_desired) {
            const bool newlyDesired =
                previousDesired.find(coord) == previousDesired.end();
            if (newlyDesired) {
                beginCameraVisibilityTrace(coord);
            }
            if (previouslyQueued.find(coord) != previouslyQueued.end() ||
                newlyDesired) {
                queueLoadGen(coord);
            }
        }
        reconcileConfigRetiredWork(schedulerCoordinatesInspected);
        for (const ChunkCoord& coord : previousDesired) {
            if (m_desiredSet.find(coord) == m_desiredSet.end()) {
                bool retainedMesh =
                    distanceSquared(center, coord) <= unloadRadiusSq &&
                    dirtyMeshPriority(coord).has_value();
                erasePendingGeneration(coord);
                if (m_versionReplacementRetries.erase(coord) > 0) {
                    m_evictionRetryAfter.erase(coord);
                    eraseFailure(
                        m_evictionErrors, m_evictionFailureVersion, coord);
                    if (m_evictionRetryAfter.empty()) {
                        m_nextEvictionRetrySequence = 0;
                    }
                }
                m_versionReplacementWaiting.erase(coord);
                if (!retainedMesh) {
                    m_meshDependencyWaiting.erase(coord);
                    retireReplacementPending(coord);
                    m_priorityMeshRequests.erase(coord);
                }
                eraseFailure(
                    m_generationErrors, m_generationFailureVersion, coord);
                eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
                if (!retainedMesh) {
                    eraseFailure(m_meshErrors, m_meshFailureVersion, coord);
                }
                completePendingVisibilityTrace(
                    coord,
                    ChunkVisibilityLifecycleKind::CameraDemand,
                    ChunkVisibilityOutcome::CameraLeft);
                auto flightIt = m_meshInFlight.find(coord);
                if (flightIt != m_meshInFlight.end() &&
                    flightIt->second.visibilityKind ==
                        ChunkVisibilityLifecycleKind::CameraDemand) {
                    completeInFlightVisibilityTrace(
                        flightIt->second,
                        ChunkVisibilityOutcome::CameraLeft);
                }
                if (m_meshStore && m_visibilityTracer &&
                    m_visibilityTracer->traces(coord)) {
                    m_meshStore->endCameraVisibilityTrace(coord);
                }
                auto stateIt = m_states.find(coord);
                if (stateIt != m_states.end()) {
                    if (stateIt->second == ChunkState::QueuedGen) {
                        retireGeneration(coord);
                    } else if ((stateIt->second == ChunkState::QueuedMesh ||
                                stateIt->second == ChunkState::GenerationFailed) &&
                               !retainedMesh) {
                        m_states.erase(stateIt);
                    }
                }
                cancelPendingLoad(coord);
                queueLoadedNeighbors(coord);
            }
        }
        m_desiredSetRebuildPending = false;
    }

    residentEvictionCoordinatesInspected +=
        reconcileDeferredWorldBounds();

    deferredEvictionCoordinatesInspected +=
        retryDeferredEvictions(center, unloadRadiusSq);

    if (!m_chunkLoadDrain && !m_loadPending.empty()) {
        for (auto it = m_loadPending.begin(); it != m_loadPending.end(); ) {
            ++schedulerCoordinatesInspected;
            ChunkCoord coord = it->first;
            bool resident = m_chunkManager->getChunk(coord) != nullptr;
            bool resolved = resident;
            if (!resolved && m_chunkPending) {
                resolved = !m_chunkPending(coord);
            } else if (!resolved) {
                queueLoadGen(coord);
                ++it;
                continue;
            }
            if (resolved) {
                ++it;
                if (resident) {
                    observeVisibilityDataReady(
                        coord, ChunkVisibilityOrigin::Persisted);
                    cancelPendingLoad(coord);
                    observeVisibilityNeighborReadiness(coord);
                } else {
                    m_loadPending.erase(coord);
                }
                queueLoadGen(coord);
            } else {
                ++it;
            }
        }
    }

    auto consumeDirtyMeshNotifications = [&]() {
        for (const ChunkCoord& coord :
             m_chunkManager->consumeDirtyMeshNotifications()) {
            if (m_chunkManager->getChunk(coord)) {
                cancelPendingLoad(coord);
                const auto [residentIt, inserted] =
                    m_streamedResidents.insert(coord);
                (void)residentIt;
                const int currentUnloadDistance = std::max(
                    std::max(0, m_config.viewDistanceChunks),
                    m_config.unloadDistanceChunks);
                const bool outsideRetention = m_lastCenter &&
                    distanceSquared(*m_lastCenter, coord) >
                        currentUnloadDistance * currentUnloadDistance;
                if (inserted && m_generator &&
                    (!chunkIntersectsWorldBounds(coord) ||
                     outsideRetention)) {
                    const bool cursorAlreadyPassed =
                        m_worldBoundsReconciliation &&
                        m_worldBoundsReconciliation->deferredCursor &&
                        !(m_worldBoundsReconciliation->deferredCursor.value() <
                          coord);
                    if (!m_worldBoundsReconciliation) {
                        m_worldBoundsReconciliation =
                            PendingWorldBoundsReconciliation{
                                .replacement = m_generator->definition().bounds,
                                .replacementSemanticsVersion =
                                    m_generator->semanticsVersion()
                            };
                    } else {
                        m_worldBoundsReconciliation->replacement =
                            m_generator->definition().bounds;
                        m_worldBoundsReconciliation->replacementSemanticsVersion =
                            m_generator->semanticsVersion();
                    }
                    m_worldBoundsReconciliation->retentionCenter = m_lastCenter;
                    m_worldBoundsReconciliation->retentionRadiusSquared =
                        currentUnloadDistance * currentUnloadDistance;
                    m_worldBoundsReconciliation->revisitFromStart =
                        m_worldBoundsReconciliation->revisitFromStart ||
                        cursorAlreadyPassed;
                }
            }
            queueDirtyMesh(coord);
        }
    };
    consumeDirtyMeshNotifications();

    if (!m_loadGenQueue.empty()) {
        PROFILE_SCOPE("Streaming/Update/LoadGen");
        size_t budget = (m_config.updateBudgetPerFrame <= 0)
            ? std::numeric_limits<size_t>::max()
            : static_cast<size_t>(m_config.updateBudgetPerFrame);
        size_t queued = 0;
        size_t scanned = 0;
        size_t candidates = m_loadGenQueue.size();
        while (queued < budget && scanned < candidates && !m_loadGenQueue.empty()) {
            ChunkCoord coord = m_loadGenQueue.front();
            m_loadGenQueue.pop_front();
            ++scanned;
            ++schedulerCoordinatesInspected;
            if (m_loadGenQueued.erase(coord) == 0) {
                continue;
            }

            if (m_desiredSet.find(coord) == m_desiredSet.end()) {
                continue;
            }

            ChunkState state = ChunkState::Missing;
            auto stateIt = m_states.find(coord);
            if (stateIt != m_states.end()) {
                state = stateIt->second;
            }

            Chunk* chunk = m_chunkManager->getChunk(coord);
            ChunkLoadRequestResult loadResult = ChunkLoadRequestResult::Missing;
            ChunkLoadRequestId loadRequestId = 0;
            if (!chunk && state != ChunkState::QueuedGen && m_chunkLoader) {
                markVisibilityStage(
                    coord, ChunkVisibilityStage::DataRequest);
                auto pendingIt = m_loadPending.find(coord);
                bool wasPending = pendingIt != m_loadPending.end();
                loadRequestId = wasPending
                    ? pendingIt->second
                    : nextLoadRequestId();
                loadResult = m_chunkLoader({coord, loadRequestId});
                if (loadResult != ChunkLoadRequestResult::Missing && !wasPending) {
                    ++m_workMetrics.chunkLoadRequestsStarted;
                }
                chunk = m_chunkManager->getChunk(coord);
                if (chunk) {
                    cancelPendingLoad(coord);
                    observeVisibilityDataReady(
                        coord, ChunkVisibilityOrigin::Persisted);
                    observeVisibilityNeighborReadiness(coord);
                }
            }

            if (chunk) {
                cancelPendingLoad(coord);
                if (m_generator &&
                    chunk->worldGenVersion() != m_generator->semanticsVersion()) {
                    if (!evictChunk(coord, true)) {
                        continue;
                    }
                    queuePendingGeneration(coord);
                    ++queued;
                    continue;
                }
                m_versionReplacementWaiting.erase(coord);
                eraseFailure(
                    m_evictionErrors, m_evictionFailureVersion, coord);

                m_cache.touch(coord);
                m_streamedResidents.insert(coord);
                cacheEvictionNeeded = true;
                bool hasMesh = m_meshStore && m_meshStore->contains(coord);
                bool isMeshed = hasMesh || state == ChunkState::ReadyMesh;
                if (stateIt == m_states.end() || state == ChunkState::QueuedGen) {
                    state = isMeshed ? ChunkState::ReadyMesh : ChunkState::ReadyData;
                    m_states[coord] = state;
                    queueLoadedNeighbors(coord);
                }

                if (chunk->isEmpty()) {
                    if (m_meshStore) {
                        m_meshStore->remove(coord);
                    }
                    m_worldBoundsSuppressedMeshes.erase(coord);
                    chunk->clearDirty();
                    m_priorityMeshRequests.erase(coord);
                    retirePendingMesh(coord);
                    retireReplacementPending(coord);
                    m_states[coord] = ChunkState::ReadyMesh;
                    m_countedMeshRetryRevisions.erase(coord);
                    completePendingVisibilityTrace(
                        coord,
                        ChunkVisibilityOutcome::VoxelEmpty);
                    continue;
                }

                if (isMeshed && chunk->isDirty()) {
                    queueDirtyMesh(coord);
                }

                if (!isMeshed && state != ChunkState::QueuedMesh) {
                    bool coordinateMeshInFlight =
                        m_meshInFlight.find(coord) != m_meshInFlight.end();
                    bool neighborsLoaded = hasAllNeighborsLoaded(coord);
                    if (coordinateMeshInFlight) {
                        auto flightIt = m_meshInFlight.find(coord);
                        if (flightIt != m_meshInFlight.end() &&
                            flightIt->second.obsolete && neighborsLoaded &&
                            queuePendingMesh(
                                coord, MeshRequestKind::Missing)) {
                            ++queued;
                        }
                        continue;
                    }
                    if (neighborsLoaded) {
                        if (queuePendingMesh(
                                coord, MeshRequestKind::Missing)) {
                            ++queued;
                        }
                    } else {
                        waitForMeshDependencies(coord);
                    }
                }
                continue;
            }

            if (state == ChunkState::QueuedGen) {
                continue;
            }

            if (loadResult != ChunkLoadRequestResult::Missing) {
                m_loadPending.insert_or_assign(coord, loadRequestId);
                ++queued;
                continue;
            }
            eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
            if (m_chunkPending && m_chunkPending(coord)) {
                markVisibilityStage(
                    coord, ChunkVisibilityStage::DataRequest);
                if (loadRequestId == 0) {
                    loadRequestId = nextLoadRequestId();
                }
                m_loadPending.insert_or_assign(coord, loadRequestId);
                ++queued;
                continue;
            }

            queuePendingGeneration(coord);
            ++queued;
        }
    }

    dispatchPendingGenerations(schedulerCoordinatesInspected);
    dispatchPendingMeshes(schedulerCoordinatesInspected);

    if (rebuildDesired && demandShapeChanged &&
        !boundedResidentReconciliation) {
        PROFILE_SCOPE("Streaming/Update/Evict");
        std::vector<ChunkCoord> toEvict;
        m_chunkManager->forEachChunk([&](ChunkCoord coord, const Chunk&) {
            ++residentEvictionCoordinatesInspected;
            if (!chunkIntersectsWorldBounds(coord) ||
                distanceSquared(center, coord) > unloadRadiusSq) {
                toEvict.push_back(coord);
            }
        });
        for (const ChunkCoord& coord : toEvict) {
            if (evictChunk(coord)) {
                m_cache.erase(coord);
            }
        }
    }

    if (cacheEvictionNeeded) {
        PROFILE_SCOPE("Streaming/Update/CacheEvict");
        m_cache.evict(
            m_desiredSet,
            [this](ChunkCoord coord) { return evictChunk(coord); });
        cacheEvictionCoordinatesInspected = m_cache.lastEvictionInspections();
    }
    consumeDirtyMeshNotifications();
    refreshVisibilityDependencySnapshot();

    m_workMetrics.lastUpdateDesiredBuildCoordinatesInspected =
        desiredBuildCoordinatesInspected;
    m_workMetrics.lastUpdateDesiredBuildCoordinatesSkippedByWorldBounds =
        desiredBuildCoordinatesSkippedByWorldBounds;
    m_workMetrics.lastUpdateCacheEvictionCoordinatesInspected =
        cacheEvictionCoordinatesInspected;
    m_workMetrics.lastUpdateResidentEvictionCoordinatesInspected =
        residentEvictionCoordinatesInspected;
    m_workMetrics.lastUpdateDeferredEvictionCoordinatesInspected =
        deferredEvictionCoordinatesInspected;
    m_workMetrics.desiredBuildCoordinatesInspected +=
        desiredBuildCoordinatesInspected;
    m_workMetrics.desiredBuildCoordinatesSkippedByWorldBounds +=
        desiredBuildCoordinatesSkippedByWorldBounds;
    m_workMetrics.schedulerCoordinatesInspected += schedulerCoordinatesInspected;
    m_workMetrics.lastUpdateSchedulerCoordinatesInspected =
        m_workMetrics.schedulerCoordinatesInspected -
        schedulerCoordinatesInspectedBeforeUpdate;
    m_workMetrics.cacheEvictionCoordinatesInspected +=
        cacheEvictionCoordinatesInspected;
    m_workMetrics.residentEvictionCoordinatesInspected +=
        residentEvictionCoordinatesInspected;
    m_workMetrics.deferredEvictionCoordinatesInspected +=
        deferredEvictionCoordinatesInspected;
    StreamingDiagnosticSnapshot afterUpdate = collectDiagnostics();
    m_workObservedThisUpdate = m_workObservedThisUpdate || !afterUpdate.workEmpty();
    m_workStartedThisUpdate =
        beforeUpdate.generation.started != afterUpdate.generation.started ||
        beforeUpdate.chunkLoad.started != afterUpdate.chunkLoad.started ||
        beforeUpdate.mesh.started != afterUpdate.mesh.started;
    refreshDiagnostics(false);
}

ChunkCoord ChunkStreamer::cameraToChunk(const glm::vec3& cameraPos) const {
    return worldToChunk(
        static_cast<int>(std::floor(cameraPos.x)),
        static_cast<int>(std::floor(cameraPos.y)),
        static_cast<int>(std::floor(cameraPos.z))
    );
}

void ChunkStreamer::processCompletions() {
    if (!m_chunkManager) {
        return;
    }

    StreamingDiagnosticSnapshot beforeCompletions = collectDiagnostics();
    m_workObservedThisUpdate =
        m_workObservedThisUpdate || !beforeCompletions.workEmpty();

    size_t loadBudget = (m_config.loadApplyBudgetPerFrame <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.loadApplyBudgetPerFrame);
    if (m_chunkLoadDrain) {
        PROFILE_SCOPE("Streaming/LoadDrain");
        for (const ChunkLoadCompletion& completion : m_chunkLoadDrain(loadBudget)) {
            auto pendingIt = m_loadPending.find(completion.coord);
            if (pendingIt == m_loadPending.end() ||
                pendingIt->second != completion.requestId) {
                continue;
            }
            m_loadPending.erase(pendingIt);
            if (!hasDirectStreamingDemand(completion.coord)) {
                eraseFailure(
                    m_loadErrors, m_loadFailureVersion, completion.coord);
                continue;
            }
            if (completion.outcome == ChunkLoadOutcome::Failed) {
                m_priorityMeshRequests.erase(completion.coord);
                setFailure(
                    m_loadErrors,
                    m_loadFailureVersion,
                    completion.coord,
                    failureDiagnostic(
                        "load", completion.coord, completion.error));
                completePendingVisibilityTrace(
                    completion.coord,
                    ChunkVisibilityOutcome::Failed);
                continue;
            }
            eraseFailure(
                m_loadErrors, m_loadFailureVersion, completion.coord);
            if (completion.outcome == ChunkLoadOutcome::Loaded &&
                m_visibilityTracer &&
                m_chunkManager->getChunk(completion.coord)) {
                observeVisibilityDataReady(
                    completion.coord, ChunkVisibilityOrigin::Persisted);
                observeVisibilityNeighborReadiness(completion.coord);
            }
            queueLoadGen(completion.coord);
        }
    }
    size_t budget = (m_config.applyBudgetPerFrame <= 0)
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(m_config.applyBudgetPerFrame);
    {
        PROFILE_SCOPE("Streaming/GenApply");
        applyGenCompletions(budget);
    }
    uint64_t schedulerCoordinatesInspected = 0;
    dispatchPendingGenerations(schedulerCoordinatesInspected);
    m_workMetrics.schedulerCoordinatesInspected +=
        schedulerCoordinatesInspected;
    {
        PROFILE_SCOPE("Streaming/MeshApply");
        applyMeshCompletions(budget);
    }

    StreamingDiagnosticSnapshot afterCompletions = collectDiagnostics();
    m_workObservedThisUpdate =
        m_workObservedThisUpdate || !afterCompletions.workEmpty();
    m_workStartedThisUpdate = m_workStartedThisUpdate ||
        beforeCompletions.generation.started != afterCompletions.generation.started ||
        beforeCompletions.chunkLoad.started != afterCompletions.chunkLoad.started ||
        beforeCompletions.mesh.started != afterCompletions.mesh.started;

    bool advanceWindow = m_lifecycleUpdateSequence != m_streamingUpdateSequence;
    refreshDiagnostics(advanceWindow);
    if (advanceWindow) {
        m_lifecycleUpdateSequence = m_streamingUpdateSequence;
    }
}

void ChunkStreamer::getDebugStates(std::vector<DebugChunkState>& out,
                                   ChunkCoord center,
                                   int radius) const {
    out.clear();
    radius = std::clamp(
        radius, 0, Preferences::kMaximumViewDistanceChunks);

    std::optional<ChunkVisibilityTraceRecord> latestTrace;
    if (m_visibilityTracer) {
        const ChunkCoord traceCoord = m_visibilityTracer->coord();
        const int64_t dx = static_cast<int64_t>(traceCoord.x) - center.x;
        const int64_t dy = static_cast<int64_t>(traceCoord.y) - center.y;
        const int64_t dz = static_cast<int64_t>(traceCoord.z) - center.z;
        if (std::abs(dx) <= radius && std::abs(dy) <= radius &&
            std::abs(dz) <= radius) {
            latestTrace = m_visibilityTracer->latestRecord();
        }
    }

    const int64_t minX = static_cast<int64_t>(center.x) - radius;
    const int64_t maxX = static_cast<int64_t>(center.x) + radius;
    const int64_t minY = static_cast<int64_t>(center.y) - radius;
    const int64_t maxY = static_cast<int64_t>(center.y) + radius;
    const int64_t minZ = static_cast<int64_t>(center.z) - radius;
    const int64_t maxZ = static_cast<int64_t>(center.z) + radius;
    for (int64_t z = minZ; z <= maxZ; ++z) {
        if (z < std::numeric_limits<int>::min() ||
            z > std::numeric_limits<int>::max()) {
            continue;
        }
        for (int64_t y = minY; y <= maxY; ++y) {
            if (y < std::numeric_limits<int>::min() ||
                y > std::numeric_limits<int>::max()) {
                continue;
            }
            for (int64_t x = minX; x <= maxX; ++x) {
                if (x < std::numeric_limits<int>::min() ||
                    x > std::numeric_limits<int>::max()) {
                    continue;
                }
                const ChunkCoord coord{
                    static_cast<int>(x),
                    static_cast<int>(y),
                    static_cast<int>(z)};
                const auto stateIt = m_states.find(coord);
                const auto loadIt = m_loadPending.find(coord);
                const auto pendingMeshIt = m_pendingMeshes.find(coord);
                const auto meshFlightIt = m_meshInFlight.find(coord);
                const auto retiredIt = m_configRetiredWork.find(coord);
                const bool tracked =
                    stateIt != m_states.end() ||
                    loadIt != m_loadPending.end() ||
                    m_loadGenQueued.find(coord) != m_loadGenQueued.end() ||
                    m_pendingGenerations.find(coord) !=
                        m_pendingGenerations.end() ||
                    m_meshDependencyWaiting.find(coord) !=
                        m_meshDependencyWaiting.end() ||
                    pendingMeshIt != m_pendingMeshes.end() ||
                    meshFlightIt != m_meshInFlight.end() ||
                    retiredIt != m_configRetiredWork.end() ||
                    m_generationErrors.find(coord) != m_generationErrors.end() ||
                    m_loadErrors.find(coord) != m_loadErrors.end() ||
                    m_meshErrors.find(coord) != m_meshErrors.end() ||
                    m_evictionErrors.find(coord) != m_evictionErrors.end();
                if (!tracked) {
                    continue;
                }
                const auto meshSnapshot = m_meshStore
                    ? m_meshStore->snapshot(coord)
                    : std::nullopt;

                DebugChunkState debug;
                debug.coord = coord;
                Chunk* chunk = m_chunkManager
                    ? m_chunkManager->getChunk(coord)
                    : nullptr;
                if (chunk) {
                    debug.voxelOccupancy = chunk->isEmpty()
                        ? DebugVoxelOccupancy::Empty
                        : DebugVoxelOccupancy::Nonempty;
                }
                if (meshSnapshot) {
                    debug.installedGeometry = meshSnapshot->empty
                        ? DebugInstalledGeometry::Empty
                        : DebugInstalledGeometry::Nonempty;
                    debug.installedGeometryRevision =
                        meshSnapshot->revision.value;
                    debug.drawEvidence = meshSnapshot->empty
                        ? DebugDrawEvidence::NotApplicable
                        : DebugDrawEvidence::NotDrawn;
                }

                if ((stateIt != m_states.end() &&
                     stateIt->second == ChunkState::GenerationFailed) ||
                    m_generationErrors.find(coord) !=
                        m_generationErrors.end()) {
                    debug.failure = DebugFailure::Generation;
                } else if (m_loadErrors.find(coord) != m_loadErrors.end()) {
                    debug.failure = DebugFailure::Load;
                } else if ((stateIt != m_states.end() &&
                            stateIt->second == ChunkState::MeshFailed) ||
                           m_meshErrors.find(coord) != m_meshErrors.end()) {
                    debug.failure = DebugFailure::Mesh;
                } else if (m_evictionErrors.find(coord) !=
                           m_evictionErrors.end()) {
                    debug.failure = DebugFailure::Eviction;
                }

                bool remeshPending = chunk && chunk->isDirty() &&
                    (meshSnapshot.has_value() ||
                     (stateIt != m_states.end() &&
                      stateIt->second == ChunkState::ReadyMesh));
                remeshPending = remeshPending ||
                    (pendingMeshIt != m_pendingMeshes.end() &&
                     pendingMeshIt->second.kind == MeshRequestKind::Dirty);
                remeshPending = remeshPending ||
                    (meshFlightIt != m_meshInFlight.end() &&
                     (meshFlightIt->second.kind == MeshRequestKind::Dirty ||
                      meshFlightIt->second.replacementPending));
                remeshPending = remeshPending ||
                    (retiredIt != m_configRetiredWork.end() &&
                     retiredIt->second == ConfigRetiredWorkKind::DirtyMesh);
                const bool suppressedByWorldBounds =
                    !chunkIntersectsWorldBounds(coord) &&
                    m_worldBoundsSuppressedMeshes.find(coord) !=
                        m_worldBoundsSuppressedMeshes.end();
                if (suppressedByWorldBounds) {
                    remeshPending = false;
                }
                if (remeshPending) {
                    debug.remeshIntent = DebugRemeshIntent::Pending;
                }

                const auto reconciliationOwner = [&]() {
                    switch (classifyPendingWork(coord)) {
                        case PendingWorkKind::Generation:
                            return DebugPipelineOwner::WaitingForData;
                        case PendingWorkKind::Mesh: {
                            if (chunk && !chunk->isEmpty() &&
                                !hasAllNeighborsLoaded(coord)) {
                                return DebugPipelineOwner::WaitingForNeighbors;
                            }
                            return remeshPending
                                ? DebugPipelineOwner::DirtyRemesh
                                : DebugPipelineOwner::MeshScheduler;
                        }
                        case PendingWorkKind::None:
                            return DebugPipelineOwner::Complete;
                    }
                    return DebugPipelineOwner::Complete;
                };

                if (debug.failure != DebugFailure::None) {
                    debug.pipelineOwner = DebugPipelineOwner::TerminalFailure;
                } else if (m_meshDependencyWaiting.find(coord) !=
                           m_meshDependencyWaiting.end()) {
                    debug.pipelineOwner =
                        DebugPipelineOwner::WaitingForNeighbors;
                } else if (pendingMeshIt != m_pendingMeshes.end()) {
                    debug.pipelineOwner = DebugPipelineOwner::MeshScheduler;
                } else if (meshFlightIt != m_meshInFlight.end() &&
                           !meshFlightIt->second.obsolete) {
                    debug.pipelineOwner = DebugPipelineOwner::MeshWork;
                } else if (meshFlightIt != m_meshInFlight.end() &&
                           meshFlightIt->second.replacementPending) {
                    debug.pipelineOwner = DebugPipelineOwner::DirtyRemesh;
                } else if (loadIt != m_loadPending.end() ||
                           m_pendingGenerations.find(coord) !=
                               m_pendingGenerations.end() ||
                           (stateIt != m_states.end() &&
                            stateIt->second == ChunkState::QueuedGen)) {
                    debug.pipelineOwner = DebugPipelineOwner::WaitingForData;
                } else if (retiredIt != m_configRetiredWork.end() ||
                           m_loadGenQueued.find(coord) !=
                               m_loadGenQueued.end()) {
                    debug.pipelineOwner = reconciliationOwner();
                } else if (remeshPending) {
                    debug.pipelineOwner = DebugPipelineOwner::DirtyRemesh;
                } else if (stateIt != m_states.end() &&
                           stateIt->second == ChunkState::ReadyData) {
                    debug.pipelineOwner = hasAllNeighborsLoaded(coord)
                        ? DebugPipelineOwner::MeshScheduler
                        : DebugPipelineOwner::WaitingForNeighbors;
                } else {
                    debug.pipelineOwner = DebugPipelineOwner::Complete;
                }

                switch (debug.pipelineOwner) {
                    case DebugPipelineOwner::WaitingForData:
                        debug.state = DebugState::WaitingForData;
                        break;
                    case DebugPipelineOwner::WaitingForNeighbors:
                        debug.state = DebugState::WaitingForNeighbors;
                        break;
                    case DebugPipelineOwner::MeshScheduler:
                        debug.state = remeshPending
                            ? DebugState::DirtyRemeshPending
                            : DebugState::MeshSchedulerWait;
                        break;
                    case DebugPipelineOwner::MeshWork:
                        debug.state = DebugState::MeshSubmittedOrBuilding;
                        break;
                    case DebugPipelineOwner::DirtyRemesh:
                        debug.state = DebugState::DirtyRemeshPending;
                        break;
                    case DebugPipelineOwner::TerminalFailure:
                        debug.state = DebugState::TerminalFailure;
                        break;
                    case DebugPipelineOwner::Complete:
                        if (suppressedByWorldBounds) {
                            debug.state =
                                DebugState::SuppressedByWorldBounds;
                        } else if (debug.voxelOccupancy ==
                            DebugVoxelOccupancy::Empty) {
                            debug.state = DebugState::VoxelEmpty;
                        } else if (debug.installedGeometry ==
                                   DebugInstalledGeometry::Empty) {
                            debug.state = DebugState::AcceptedEmptyGeometry;
                        } else if (debug.installedGeometry ==
                                   DebugInstalledGeometry::Nonempty) {
                            debug.state =
                                DebugState::AcceptedNonemptyGeometry;
                        } else {
                            debug.state = DebugState::WaitingForNeighbors;
                            debug.pipelineOwner =
                                DebugPipelineOwner::WaitingForNeighbors;
                        }
                        break;
                }

                if (latestTrace && latestTrace->key.coord == coord) {
                    debug.historicalTraceKey = latestTrace->key;
                    debug.historicalTraceKind = latestTrace->kind;
                    debug.historicalTraceOutcome = latestTrace->outcome;
                    debug.historicalTraceDrawOutcome =
                        latestTrace->drawOutcome;
                }
                out.push_back(std::move(debug));
            }
        }
    }
}

void ChunkStreamer::reset() {
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                ChunkVisibilityOutcome::Reset);
            break;
        }
    }
    retireAllGenerations();

    uint64_t nextEpoch = m_workEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nextEpoch == 0) {
        m_workEpoch.store(1, std::memory_order_relaxed);
    }
    m_states.clear();
    m_worldBoundsSuppressedMeshes.clear();
    for (auto& entry : m_meshInFlight) {
        entry.second.obsolete = true;
        setReplacementPending(entry.first, entry.second, false);
        entry.second.prioritized = false;
        completeInFlightVisibilityTrace(
            entry.second,
            ChunkVisibilityOutcome::Reset);
    }
    m_countedMeshRetryRevisions.clear();
    m_cache = ChunkCache();
    m_cache.setMaxChunks(m_config.maxResidentChunks);
    m_desired.clear();
    m_desiredSet.clear();
    m_desiredPriority.clear();
    m_pendingMeshQueues = {};
    m_pendingMeshes.clear();
    m_priorityMeshRequests.clear();
    m_evictionRetryAfter.clear();
    m_versionReplacementRetries.clear();
    m_versionReplacementWaiting.clear();
    clearFailures(m_generationErrors, m_generationFailureVersion);
    clearFailures(m_loadErrors, m_loadFailureVersion);
    clearFailures(m_meshErrors, m_meshFailureVersion);
    clearFailures(m_evictionErrors, m_evictionFailureVersion);
    m_nextEvictionRetrySequence = 0;
    m_evictionRetryScanCursor.reset();
    m_evictionRetryScanActive = false;
    m_loadGenQueue.clear();
    m_loadGenQueued.clear();
    m_pendingGenerationQueue.clear();
    m_pendingGenerations.clear();
    m_meshDependencyWaiting.clear();
    m_configRetiredWork.clear();
    if (m_chunkLoadCancel) {
        for (const auto& pending : m_loadPending) {
            m_chunkLoadCancel(pending.first);
        }
    }
    m_loadPending.clear();
    m_lastCenter.reset();
    m_lastViewDistance = -1;
    m_lastUnloadDistance = -1;
    m_desiredSetRebuildPending = false;
    m_worldBoundsReconciliation.reset();
    m_initialStreamingBegun = false;
    m_workObservedThisUpdate = false;
    m_workStartedThisUpdate = false;
    m_nextSingleSlotMeshKind = MeshRequestKind::Missing;
    m_nextPendingMeshSequence = 1;
    m_streamingUpdateSequence = 0;
    m_lifecycleUpdateSequence = 0;
    m_replacementPendingMeshCount = 0;

    applyGenCompletions(std::numeric_limits<size_t>::max());
    applyMeshCompletions(std::numeric_limits<size_t>::max());
    refreshDiagnostics(false);
}

bool ChunkStreamer::evictChunk(ChunkCoord coord, bool versionReplacement) {
    bool replacementPending = versionReplacement ||
        m_versionReplacementRetries.find(coord) !=
            m_versionReplacementRetries.end() ||
        m_versionReplacementWaiting.find(coord) !=
            m_versionReplacementWaiting.end();
    Chunk* chunk = m_chunkManager ? m_chunkManager->getChunk(coord) : nullptr;
    if (chunk && chunk->isPersistDirty()) {
        auto retryIt = m_evictionRetryAfter.find(coord);
        if (retryIt != m_evictionRetryAfter.end() &&
            retryIt->second > m_streamingUpdateSequence) {
            return false;
        }
        if (!m_chunkEviction || !m_chunkEviction(coord)) {
            deferEviction(coord, replacementPending);
            return false;
        }
        chunk = m_chunkManager->getChunk(coord);
        if (chunk && chunk->isPersistDirty()) {
            deferEviction(coord, replacementPending);
            return false;
        }
    }

    m_evictionRetryAfter.erase(coord);
    m_versionReplacementRetries.erase(coord);
    eraseFailure(m_evictionErrors, m_evictionFailureVersion, coord);
    if (replacementPending) {
        m_versionReplacementWaiting.insert(coord);
    } else {
        m_versionReplacementWaiting.erase(coord);
    }
    if (m_meshStore) {
        m_meshStore->remove(coord);
    }
    if (std::any_of(
            m_pendingVisibilityTraces.begin(),
            m_pendingVisibilityTraces.end(),
            [&](const auto& pending) {
                return pending && pending->key.coord == coord;
            })) {
        completePendingVisibilityTrace(
            coord, ChunkVisibilityOutcome::Stale);
    }
    if (chunk) {
        chunk->setLoadedFromDisk(false);
        const bool invalidateNeighbors =
            !m_generator || chunkIntersectsWorldBounds(coord);
        m_chunkManager->unloadChunk(coord, invalidateNeighbors);
        for (int i = 0; i < DirectionCount; ++i) {
            int dx = 0;
            int dy = 0;
            int dz = 0;
            directionOffset(static_cast<Direction>(i), dx, dy, dz);
            const ChunkCoord neighbor = coord.offset(dx, dy, dz);
            if (m_pendingMeshes.find(neighbor) != m_pendingMeshes.end() &&
                !hasAllNeighborsLoaded(neighbor)) {
                waitForMeshDependencies(neighbor);
            }
        }
    }
    m_states.erase(coord);
    m_streamedResidents.erase(coord);
    m_worldBoundsSuppressedMeshes.erase(coord);
    m_configRetiredWork.erase(coord);
    retirePendingMesh(coord);
    retireReplacementPending(coord);
    m_priorityMeshRequests.erase(coord);
    m_countedMeshRetryRevisions.erase(coord);
    eraseFailure(m_generationErrors, m_generationFailureVersion, coord);
    eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
    eraseFailure(m_meshErrors, m_meshFailureVersion, coord);
    return true;
}

void ChunkStreamer::deferEviction(ChunkCoord coord, bool versionReplacement) {
    if (m_chunkManager && m_chunkManager->getChunk(coord)) {
        m_streamedResidents.insert(coord);
    }
    uint64_t retrySequence = m_streamingUpdateSequence + kEvictionRetryDelayUpdates;
    m_evictionRetryAfter[coord] = retrySequence;
    setFailure(
        m_evictionErrors,
        m_evictionFailureVersion,
        coord,
        failureDiagnostic(
            "eviction persistence", coord, "persistence did not complete"));
    if (versionReplacement) {
        m_versionReplacementRetries.insert(coord);
        m_versionReplacementWaiting.erase(coord);
    } else {
        m_versionReplacementRetries.erase(coord);
    }
    if (m_nextEvictionRetrySequence == 0 ||
        retrySequence < m_nextEvictionRetrySequence) {
        m_nextEvictionRetrySequence = retrySequence;
    }
    m_configRetiredWork.erase(coord);
}

uint64_t ChunkStreamer::retireIneligibleEvictions(ChunkCoord center,
                                                  int viewRadiusSq,
                                                  int unloadRadiusSq) {
    uint64_t inspected = 0;
    m_nextEvictionRetrySequence = 0;
    m_evictionRetryScanCursor.reset();
    m_evictionRetryScanActive = false;
    for (auto it = m_evictionRetryAfter.begin();
         it != m_evictionRetryAfter.end();) {
        ++inspected;
        const ChunkCoord coord = it->first;
        if (m_versionReplacementRetries.find(coord) !=
            m_versionReplacementRetries.end()) {
            if (m_nextEvictionRetrySequence == 0 ||
                it->second < m_nextEvictionRetrySequence) {
                m_nextEvictionRetrySequence = it->second;
            }
            ++it;
            continue;
        }

        int distSq = distanceSquared(center, coord);
        bool desired = distSq <= viewRadiusSq &&
            m_desiredSet.find(coord) != m_desiredSet.end();
        bool outsideUnloadRadius =
            !chunkIntersectsWorldBounds(coord) ||
            distSq > unloadRadiusSq;
        bool cachePressure = m_cache.maxChunks() > 0 &&
            m_cache.size() > m_cache.maxChunks() && !desired;
        if (!outsideUnloadRadius && !cachePressure) {
            const bool meshWorkEligible =
                hasMeshReconciliationWork(coord);
            eraseFailure(
                m_evictionErrors, m_evictionFailureVersion, coord);
            it = m_evictionRetryAfter.erase(it);
            if (meshWorkEligible) {
                queueDirtyMesh(coord);
            }
            continue;
        }

        if (m_nextEvictionRetrySequence == 0 ||
            it->second < m_nextEvictionRetrySequence) {
            m_nextEvictionRetrySequence = it->second;
        }
        ++it;
    }
    return inspected;
}

uint64_t ChunkStreamer::retryDeferredEvictions(ChunkCoord center,
                                               int unloadRadiusSq) {
    if (!m_evictionRetryScanActive &&
        (m_nextEvictionRetrySequence == 0 ||
         m_streamingUpdateSequence < m_nextEvictionRetrySequence)) {
        return 0;
    }

    std::vector<std::pair<ChunkCoord, bool>> due;
    if (!m_evictionRetryScanActive) {
        m_evictionRetryScanActive = true;
        m_evictionRetryScanCursor.reset();
        m_nextEvictionRetrySequence = 0;
    }
    auto it = m_evictionRetryScanCursor
        ? m_evictionRetryAfter.upper_bound(*m_evictionRetryScanCursor)
        : m_evictionRetryAfter.begin();
    uint64_t inspected = 0;
    while (it != m_evictionRetryAfter.end() &&
           inspected < kWorldBoundsReconciliationBudget) {
        const ChunkCoord coord = it->first;
        m_evictionRetryScanCursor = coord;
        ++inspected;
        if (it->second <= m_streamingUpdateSequence) {
            bool versionReplacement =
                m_versionReplacementRetries.erase(coord) > 0;
            due.emplace_back(coord, versionReplacement);
            it = m_evictionRetryAfter.erase(it);
            continue;
        }
        if (m_nextEvictionRetrySequence == 0 ||
            it->second < m_nextEvictionRetrySequence) {
            m_nextEvictionRetrySequence = it->second;
        }
        ++it;
    }
    if (it == m_evictionRetryAfter.end()) {
        m_evictionRetryScanCursor.reset();
        m_evictionRetryScanActive = false;
    }

    for (const auto& [coord, versionReplacement] : due) {
        if (versionReplacement) {
            if (m_desiredSet.find(coord) != m_desiredSet.end()) {
                m_versionReplacementWaiting.insert(coord);
                queueLoadGen(coord);
                continue;
            }
            if (evictChunk(coord, true)) {
                m_cache.erase(coord);
            }
            continue;
        }

        bool outsideUnloadRadius =
            !chunkIntersectsWorldBounds(coord) ||
            distanceSquared(center, coord) > unloadRadiusSq;
        bool cachePressure =
            m_cache.maxChunks() > 0 &&
            m_cache.size() > m_cache.maxChunks() &&
            m_desiredSet.find(coord) == m_desiredSet.end();
        if ((outsideUnloadRadius || cachePressure) && evictChunk(coord)) {
            m_cache.erase(coord);
        } else if (!outsideUnloadRadius && !cachePressure) {
            eraseFailure(
                m_evictionErrors, m_evictionFailureVersion, coord);
        }
    }
    return inspected;
}

StreamingDiagnosticSnapshot ChunkStreamer::collectDiagnostics() {
    StreamingDiagnosticSnapshot snapshot;
    size_t generationPending = m_pendingGenerations.size();
    size_t meshPending =
        m_pendingMeshes.size() + m_meshDependencyWaiting.size() +
        m_replacementPendingMeshCount;
    auto countPending = [&](ChunkCoord coord) {
        switch (classifyPendingWork(coord)) {
            case PendingWorkKind::Generation:
                ++generationPending;
                break;
            case PendingWorkKind::Mesh:
                ++meshPending;
                break;
            case PendingWorkKind::None:
                break;
        }
    };

    for (const ChunkCoord& coord : m_loadGenQueued) {
        if (hasCanonicalWorkOwner(coord, false)) {
            continue;
        }
        countPending(coord);
    }
    for (const auto& [coord, kind] : m_configRetiredWork) {
        (void)kind;
        if (hasCanonicalWorkOwner(coord)) {
            continue;
        }
        const PendingWorkKind work = classifyPendingWork(coord);
        const Chunk* chunk = m_chunkManager
            ? m_chunkManager->getChunk(coord)
            : nullptr;
        const bool retainedVoxelCleanup =
            work == PendingWorkKind::Mesh && chunk && chunk->isEmpty() &&
            m_meshStore && m_meshStore->contains(coord);
        const bool eligible = work == PendingWorkKind::Generation
            ? hasDirectStreamingDemand(coord)
            : work == PendingWorkKind::Mesh &&
                (dirtyMeshPriority(coord).has_value() ||
                 retainedVoxelCleanup);
        if (eligible) {
            countPending(coord);
        }
    }

    snapshot.generation = StreamingWorkCount{
        .pending = generationPending,
        .inFlight = m_inFlightGen,
        .started = m_workMetrics.generationJobsStarted,
        .terminalErrors = m_generationErrors.size(),
        .lastError = diagnosticForLowestCoordinate(m_generationErrors),
        .failureVersion = m_generationFailureVersion
    };
    snapshot.plannerReconciliationPending =
        (m_desiredSetRebuildPending || m_worldBoundsReconciliation) ? 1 : 0;
    snapshot.sourceResolutionPending = m_loadGenQueued.size();
    snapshot.generationSchedulerPending = m_pendingGenerations.size();
    snapshot.generationCompletionsPending = m_genComplete.size();
    snapshot.mesh = StreamingWorkCount{
        .pending = meshPending,
        .inFlight = m_inFlightMesh,
        .started = m_workMetrics.meshJobsStarted,
        .terminalErrors = m_meshErrors.size(),
        .lastError = diagnosticForLowestCoordinate(m_meshErrors),
        .failureVersion = m_meshFailureVersion
    };
    snapshot.meshCompletionsPending = m_meshComplete.size();
    snapshot.meshWorkerCount = m_meshPool
        ? m_meshPool->threadCount()
        : 0;
    snapshot.meshSubmissionLimit = meshDispatchLimit();
    if (m_chunkLoadDiagnostics) {
        ChunkLoadDiagnosticSnapshot load = m_chunkLoadDiagnostics();
        snapshot.chunkLoad = std::move(load.work);
        snapshot.regionScheduler = load.regionScheduler;
    } else {
        snapshot.chunkLoad = StreamingWorkCount{
            .pending = m_loadPending.size(),
            .inFlight = 0,
            .started = m_workMetrics.chunkLoadRequestsStarted
        };
    }
    snapshot.chunkLoad.terminalErrors += m_loadErrors.size();
    if (!m_loadErrors.empty()) {
        snapshot.chunkLoad.lastError =
            diagnosticForLowestCoordinate(m_loadErrors);
    }
    if (snapshot.chunkLoad.failureVersion != m_observedLoaderFailureVersion ||
        m_loadFailureVersion != m_observedLoadFailureVersion) {
        m_observedLoaderFailureVersion = snapshot.chunkLoad.failureVersion;
        m_observedLoadFailureVersion = m_loadFailureVersion;
        advanceFailureVersion(m_chunkLoadFailureVersion);
    }
    snapshot.chunkLoad.failureVersion = m_chunkLoadFailureVersion;
    snapshot.eviction = StreamingWorkCount{
        .pending = m_evictionRetryAfter.size() +
            m_versionReplacementWaiting.size(),
        .terminalErrors = m_evictionErrors.size(),
        .lastError = diagnosticForLowestCoordinate(m_evictionErrors),
        .failureVersion = m_evictionFailureVersion
    };
    snapshot.retiredWorkPending = m_configRetiredWork.size();
    return snapshot;
}

void ChunkStreamer::refreshDiagnostics(bool advanceWindow) {
    StreamingDiagnosticSnapshot next = collectDiagnostics();
    next.state = m_diagnostics.state;
    next.stableUpdates = m_diagnostics.stableUpdates;

    if (!m_spawnDiscoveryComplete) {
        next.state = StreamingLifecycleState::DiscoveringSpawn;
        next.stableUpdates = 0;
    } else if (!m_initialStreamingBegun) {
        next.state = StreamingLifecycleState::AwaitingInitialStream;
        next.stableUpdates = 0;
    } else if (!next.workEmpty() || m_workObservedThisUpdate || m_workStartedThisUpdate) {
        next.state = StreamingLifecycleState::Streaming;
        next.stableUpdates = 0;
    } else if (advanceWindow) {
        if (next.stableUpdates < StreamingDiagnosticSnapshot::QuiescenceUpdateWindow) {
            ++next.stableUpdates;
        }
        next.state =
            next.stableUpdates >= StreamingDiagnosticSnapshot::QuiescenceUpdateWindow
            ? StreamingLifecycleState::Quiescent
            : StreamingLifecycleState::Stabilizing;
    }

    m_diagnostics = next;
}

void ChunkStreamer::applyGenCompletions(size_t budget) {
    size_t applied = 0;
    GenResult genResult;
    while (applied < budget && m_genComplete.tryPop(genResult)) {
        const bool ownerSettled = settleGenerationOwner(
            genResult.coord, genResult.flight, false);
        const auto handOffReplacement = [&]() {
            if (ownerSettled) {
                activatePendingGeneration(genResult.coord);
            }
        };
        if (!ownerSettled) {
            continue;
        }

        if (genResult.workEpoch !=
            m_workEpoch.load(std::memory_order_relaxed)) {
            handOffReplacement();
            continue;
        }
        if (!m_generator ||
            genResult.worldGenVersion != m_generator->semanticsVersion()) {
            handOffReplacement();
            continue;
        }

        if (genResult.cancelled || (genResult.flight &&
            genResult.flight->cancelled.load(std::memory_order_relaxed))) {
            handOffReplacement();
            continue;
        }

        if (!hasDirectStreamingDemand(genResult.coord)) {
            continue;
        }

        auto stateIt = m_states.find(genResult.coord);
        if (stateIt == m_states.end() || stateIt->second != ChunkState::QueuedGen) {
            continue;
        }

        if (genResult.failed) {
            stateIt->second = ChunkState::GenerationFailed;
            m_priorityMeshRequests.erase(genResult.coord);
            ++m_workMetrics.generationJobsFailed;
            setFailure(
                m_generationErrors,
                m_generationFailureVersion,
                genResult.coord,
                failureDiagnostic(
                    "generation", genResult.coord, genResult.error));
            spdlog::error("Chunk generation failed at ({}, {}, {}): {}",
                          genResult.coord.x,
                          genResult.coord.y,
                          genResult.coord.z,
                          genResult.error);
            completePendingVisibilityTrace(
                genResult.coord,
                ChunkVisibilityOutcome::Failed);
            ++applied;
            continue;
        }

        cancelPendingLoad(genResult.coord);
        eraseFailure(
            m_generationErrors, m_generationFailureVersion, genResult.coord);
        eraseFailure(m_loadErrors, m_loadFailureVersion, genResult.coord);
        m_versionReplacementWaiting.erase(genResult.coord);
        Chunk& chunk = m_chunkManager->getOrCreateChunk(genResult.coord);
        m_streamedResidents.insert(genResult.coord);
        if (m_registry) {
            chunk.copyFrom(genResult.blocks, *m_registry);
        } else {
            chunk.copyFrom(genResult.blocks);
        }
        chunk.clearPersistDirty();
        chunk.setLoadedFromDisk(false);
        chunk.setWorldGenVersion(genResult.worldGenVersion);
        if (genResult.visibilityTracer && genResult.visibilityTrace) {
            genResult.visibilityTracer->markDataReady(
                *genResult.visibilityTrace,
                ChunkVisibilityOrigin::Generated);
        } else {
            observeVisibilityDataReady(
                genResult.coord, ChunkVisibilityOrigin::Generated);
        }
        observeVisibilityNeighborReadiness(genResult.coord);

        if (m_benchmark) {
            m_benchmark->addGeneration(genResult.seconds);
        }

        if (chunk.isEmpty()) {
            if (m_meshStore) {
                m_meshStore->remove(genResult.coord);
            }
            chunk.clearDirty();
            m_priorityMeshRequests.erase(genResult.coord);
            retirePendingMesh(genResult.coord);
            retireReplacementPending(genResult.coord);
            eraseFailure(m_meshErrors, m_meshFailureVersion, genResult.coord);
            stateIt->second = ChunkState::ReadyMesh;
            completePendingVisibilityTrace(
                genResult.coord,
                ChunkVisibilityOutcome::VoxelEmpty);
        } else {
            stateIt->second = ChunkState::ReadyData;
        }
        queueLoadGen(genResult.coord);
        queueLoadedNeighbors(genResult.coord);
        if (!chunk.isEmpty()) {
            m_chunkManager->invalidateFaceNeighbors(genResult.coord);
        }
        ++applied;
    }
}

void ChunkStreamer::applyMeshCompletions(size_t budget) {
    size_t applied = 0;
    MeshResult meshResult;
    while (applied < budget && m_meshComplete.tryPop(meshResult)) {
        ++m_workMetrics.meshJobsCompleted;
        auto completeVisibility = [&](ChunkVisibilityOutcome outcome) {
            if (meshResult.visibilityTracer && meshResult.visibilityTrace) {
                meshResult.visibilityTracer->complete(
                    *meshResult.visibilityTrace, outcome);
            }
        };
        auto flightIt = m_meshInFlight.find(meshResult.coord);
        if (flightIt == m_meshInFlight.end() ||
            flightIt->second.requestId != meshResult.requestId) {
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            queueLoadGen(meshResult.coord);
            continue;
        }

        MeshInFlight flight = flightIt->second;
        setReplacementPending(meshResult.coord, flightIt->second, false);
        if (m_inFlightMesh > 0) {
            --m_inFlightMesh;
        }
        if (flight.kind == MeshRequestKind::Missing) {
            if (m_inFlightMeshMissing > 0) {
                --m_inFlightMeshMissing;
            }
        } else if (m_inFlightMeshDirty > 0) {
            --m_inFlightMeshDirty;
        }
        m_meshInFlight.erase(flightIt);

        auto wakeReplacement = [&]() {
            if (!flight.replacementPending) {
                return;
            }
            if (!hasEligibleMeshWork(meshResult.coord)) {
                m_priorityMeshRequests.erase(meshResult.coord);
                return;
            }
            queueDirtyMesh(meshResult.coord, flight.prioritized);
        };

        if (flight.obsolete ||
            flight.workEpoch != m_workEpoch.load(std::memory_order_relaxed) ||
            meshResult.workEpoch != flight.workEpoch ||
            !dirtyMeshPriority(meshResult.coord)) {
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            wakeReplacement();
            continue;
        }

        auto stateIt = m_states.find(meshResult.coord);
        if (stateIt == m_states.end() || stateIt->second != ChunkState::QueuedMesh) {
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            queueLoadGen(meshResult.coord);
            continue;
        }

        Chunk* chunk = m_chunkManager->getChunk(meshResult.coord);
        if (!chunk) {
            m_states.erase(meshResult.coord);
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            continue;
        }

        uint32_t currentRevision = chunk->meshRevision();
        bool replaced = chunk->m_instanceId != meshResult.chunkInstanceId;
        if (replaced || currentRevision != meshResult.revision) {
            if (replaced || flight.observedRevision != currentRevision) {
                ++m_workMetrics.meshInvalidations;
                ++m_workMetrics.meshRequestsCoalesced;
            }
            if (!chunk->isEmpty()) {
                chunk->markDirty();
            }
            if (flight.kind == MeshRequestKind::Dirty) {
                m_countedMeshRetryRevisions[meshResult.coord] = currentRevision;
            }
            stateIt->second = ChunkState::ReadyData;
            ++m_workMetrics.meshJobsRejectedStale;
            completeVisibility(ChunkVisibilityOutcome::Stale);
            queueLoadGen(meshResult.coord);
            if (replaced) {
                m_priorityMeshRequests.erase(meshResult.coord);
            }
            queueDirtyMesh(
                meshResult.coord, replaced ? false : flight.prioritized);
            continue;
        }

        std::optional<ChunkVisibilityTraceLink> visibilityTrace;
        if (flight.visibilityTracer && flight.visibilityTrace) {
            visibilityTrace = ChunkVisibilityTraceLink{
                *flight.visibilityTrace,
                flight.visibilityKind,
                flight.visibilityTracer
            };
        }

        if (meshResult.failed) {
            chunk->m_dirty = true;
            stateIt->second = ChunkState::MeshFailed;
            retirePendingMesh(meshResult.coord);
            m_priorityMeshRequests.erase(meshResult.coord);
            ++m_workMetrics.meshJobsFailed;
            completeVisibility(ChunkVisibilityOutcome::Failed);
            setFailure(
                m_meshErrors,
                m_meshFailureVersion,
                meshResult.coord,
                failureDiagnostic(
                    "mesh build", meshResult.coord, meshResult.error));
            spdlog::error("Chunk mesh build failed at ({}, {}, {}): {}",
                          meshResult.coord.x,
                          meshResult.coord.y,
                          meshResult.coord.z,
                          meshResult.error);
            ++applied;
            continue;
        }

        if (meshResult.empty) {
            if (m_meshStore) {
                if (chunk->isEmpty()) {
                    m_meshStore->remove(meshResult.coord);
                } else {
                    m_meshStore->set(
                        meshResult.coord,
                        std::move(meshResult.mesh),
                        visibilityTrace);
                }
            }
        } else if (m_meshStore) {
            m_meshStore->set(
                meshResult.coord,
                std::move(meshResult.mesh),
                visibilityTrace);
        }
        chunk->clearDirty();
        m_worldBoundsSuppressedMeshes.erase(meshResult.coord);
        eraseFailure(m_meshErrors, m_meshFailureVersion, meshResult.coord);
        stateIt->second = ChunkState::ReadyMesh;
        completeVisibility(meshResult.empty
                ? ChunkVisibilityOutcome::AcceptedEmptyGeometry
                : ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

        if (m_benchmark) {
            m_benchmark->addMesh(meshResult.seconds, meshResult.empty);
        }
        ++m_workMetrics.meshJobsAccepted;
        ++applied;
    }
}

void ChunkStreamer::cancelPendingLoad(ChunkCoord coord) {
    auto pendingIt = m_loadPending.find(coord);
    if (pendingIt == m_loadPending.end()) {
        if (m_chunkManager && m_chunkManager->getChunk(coord)) {
            eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
        }
        return;
    }
    if (m_chunkLoadCancel) {
        m_chunkLoadCancel(coord);
    }
    m_loadPending.erase(pendingIt);
    eraseFailure(m_loadErrors, m_loadFailureVersion, coord);
}

ChunkLoadRequestId ChunkStreamer::nextLoadRequestId() {
    ChunkLoadRequestId requestId = m_nextLoadRequestId++;
    if (m_nextLoadRequestId == 0) {
        m_nextLoadRequestId = 1;
    }
    return requestId;
}

void ChunkStreamer::queueLoadGen(ChunkCoord coord) {
    if (!hasDirectStreamingDemand(coord)) {
        return;
    }
    erasePendingGeneration(coord);
    if (m_pendingMeshes.find(coord) != m_pendingMeshes.end()) {
        m_configRetiredWork.erase(coord);
        m_loadGenQueued.erase(coord);
        markVisibilityMeshEligible(coord, false);
        return;
    }
    auto flightIt = m_meshInFlight.find(coord);
    if (flightIt != m_meshInFlight.end() &&
        flightIt->second.replacementPending) {
        m_configRetiredWork.erase(coord);
        m_loadGenQueued.erase(coord);
        markVisibilityMeshEligible(coord, false);
        return;
    }
    retirePendingMesh(coord);
    if (m_loadGenQueued.insert(coord).second) {
        m_loadGenQueue.push_back(coord);
    }
    if (!m_chunkManager || !m_chunkManager->getChunk(coord)) {
        markVisibilityStage(
            coord, ChunkVisibilityStage::SourceResolutionPending);
    }
    m_configRetiredWork.erase(coord);
    markVisibilityMeshEligible(coord, false);
}

void ChunkStreamer::queuePendingGeneration(ChunkCoord coord) {
    if (!hasDirectStreamingDemand(coord)) {
        return;
    }
    if (m_pendingGenerations.find(coord) != m_pendingGenerations.end()) {
        return;
    }
    markVisibilityStage(coord, ChunkVisibilityStage::DataRequest);
    markVisibilityStage(
        coord, ChunkVisibilityStage::GenerationSchedulerPending);
    const ChunkCoord camera = m_lastCenter.value_or(coord);
    const ChunkImportance importance = chunkImportance(camera, coord);
    m_pendingGenerations.emplace(coord, importance);
    const bool retainedOwner =
        m_generationFlights.find(coord) != m_generationFlights.end();
    if (!retainedOwner) {
        m_pendingGenerationQueue.insert(importance);
    }
    if (retainedOwner || m_inFlightGen >= generationDispatchLimit()) {
        markVisibilityStage(
            coord, ChunkVisibilityStage::GenerationCapacityWait);
    }
    m_configRetiredWork.erase(coord);
}

void ChunkStreamer::erasePendingGeneration(ChunkCoord coord) {
    auto pendingIt = m_pendingGenerations.find(coord);
    if (pendingIt == m_pendingGenerations.end()) {
        return;
    }
    m_pendingGenerationQueue.erase(pendingIt->second);
    m_pendingGenerations.erase(pendingIt);
}

bool ChunkStreamer::settleGenerationOwner(
    ChunkCoord coord,
    const std::shared_ptr<GenerationFlight>& flight,
    bool cancelledBeforeStart) {
    auto ownerIt = m_generationFlights.find(coord);
    if (!flight || ownerIt == m_generationFlights.end() ||
        ownerIt->second != flight) {
        return false;
    }
    m_generationFlights.erase(ownerIt);
    assert(m_inFlightGen > 0);
    --m_inFlightGen;
    if (cancelledBeforeStart) {
        ++m_workMetrics.generationJobsCancelled;
    } else {
        ++m_workMetrics.generationJobsCompleted;
    }
    return true;
}

void ChunkStreamer::activatePendingGeneration(ChunkCoord coord) {
    if (!m_generator || !hasDirectStreamingDemand(coord) ||
        (m_chunkManager && m_chunkManager->getChunk(coord)) ||
        m_loadPending.find(coord) != m_loadPending.end()) {
        erasePendingGeneration(coord);
        return;
    }
    if (m_pendingGenerations.find(coord) == m_pendingGenerations.end()) {
        // A generator replacement can settle before the owner-thread source
        // queue has revisited this coordinate. Preserve persistence/version
        // resolution instead of jumping directly back into generation.
        queueLoadGen(coord);
        return;
    }
    auto pendingIt = m_pendingGenerations.find(coord);
    m_pendingGenerationQueue.erase(pendingIt->second);
    pendingIt->second = chunkImportance(
        m_lastCenter.value_or(coord), coord);
    m_pendingGenerationQueue.insert(pendingIt->second);
}

void ChunkStreamer::retireGeneration(ChunkCoord coord) {
    auto flightIt = m_generationFlights.find(coord);
    if (flightIt == m_generationFlights.end()) {
        return;
    }

    const auto flight = flightIt->second;
    flight->cancelled.store(true, std::memory_order_relaxed);
    const bool cancelledBeforeStart =
        m_genPool && flight->executorJob &&
        m_genPool->cancel(flight->executorJob);

    auto stateIt = m_states.find(coord);
    if (stateIt != m_states.end() &&
        stateIt->second == ChunkState::QueuedGen) {
        m_states.erase(stateIt);
    }
    if (cancelledBeforeStart) {
        settleGenerationOwner(coord, flight, true);
    }
}

void ChunkStreamer::retireAllGenerations() {
    std::vector<ChunkCoord> coordinates;
    coordinates.reserve(m_generationFlights.size());
    for (const auto& [coord, flight] : m_generationFlights) {
        (void)flight;
        coordinates.push_back(coord);
    }
    for (const ChunkCoord& coord : coordinates) {
        retireGeneration(coord);
    }
}

void ChunkStreamer::waitForMeshDependencies(ChunkCoord coord) {
    if (!dirtyMeshPriority(coord)) {
        retirePendingMesh(coord);
        retireReplacementPending(coord);
        m_priorityMeshRequests.erase(coord);
        return;
    }
    auto flightIt = m_meshInFlight.find(coord);
    if (flightIt != m_meshInFlight.end() && flightIt->second.obsolete) {
        if (hasEligibleMeshWork(coord)) {
            const bool prioritized =
                m_priorityMeshRequests.find(coord) !=
                m_priorityMeshRequests.end();
            setReplacementPending(coord, flightIt->second, true);
            flightIt->second.prioritized =
                flightIt->second.prioritized || prioritized;
        } else {
            setReplacementPending(coord, flightIt->second, false);
            flightIt->second.prioritized = false;
        }
        m_priorityMeshRequests.erase(coord);
        return;
    }
    erasePendingMesh(coord);
    m_loadGenQueued.erase(coord);
    m_meshDependencyWaiting.insert(coord);
    m_configRetiredWork.erase(coord);
}

void ChunkStreamer::queueLoadedNeighbors(ChunkCoord coord) {
    if (!m_chunkManager) {
        return;
    }
    for (int i = 0; i < DirectionCount; ++i) {
        Direction dir = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(dir, dx, dy, dz);
        ChunkCoord neighbor = coord.offset(dx, dy, dz);
        if (m_chunkManager->getChunk(neighbor)) {
            if (m_meshDependencyWaiting.find(neighbor) !=
                m_meshDependencyWaiting.end()) {
                queueDirtyMesh(neighbor);
            } else if (m_desiredSet.find(neighbor) != m_desiredSet.end()) {
                queueLoadGen(neighbor);
            }
        }
    }
}

bool ChunkStreamer::hasDirectStreamingDemand(ChunkCoord coord) const {
    if (!chunkIntersectsWorldBounds(coord) ||
        m_desiredSet.find(coord) == m_desiredSet.end()) {
        return false;
    }
    if (!m_lastCenter) {
        return true;
    }
    const int viewDistance = std::max(0, m_config.viewDistanceChunks);
    return distanceSquared(*m_lastCenter, coord) <=
        viewDistance * viewDistance;
}

bool ChunkStreamer::chunkIntersectsWorldBounds(ChunkCoord coord) const {
    if (!m_generator) {
        return false;
    }
    return intersectsWorldBounds(coord, m_generator->definition().bounds);
}

std::optional<size_t> ChunkStreamer::dirtyMeshPriority(ChunkCoord coord) const {
    if (!chunkIntersectsWorldBounds(coord)) {
        return std::nullopt;
    }
    int distanceSq = 0;
    if (m_lastCenter) {
        distanceSq = distanceSquared(*m_lastCenter, coord);
    }
    auto priorityIt = m_desiredPriority.find(coord);
    const int viewDistance = std::max(0, m_config.viewDistanceChunks);
    if (priorityIt != m_desiredPriority.end() &&
        (!m_lastCenter || distanceSq <= viewDistance * viewDistance)) {
        return priorityIt->second;
    }
    const int unloadDistance = std::max(
        viewDistance, m_config.unloadDistanceChunks);
    if (m_chunkManager && m_meshStore &&
        (!m_lastCenter || distanceSq <= unloadDistance * unloadDistance)) {
        Chunk* chunk = m_chunkManager->getChunk(coord);
        if (chunk &&
            (m_meshStore->contains(coord) ||
             m_worldBoundsSuppressedMeshes.find(coord) !=
                 m_worldBoundsSuppressedMeshes.end())) {
            return m_desired.size();
        }
    }
    return std::nullopt;
}

bool ChunkStreamer::queuePendingMesh(ChunkCoord coord,
                                     MeshRequestKind kind,
                                     bool prioritize) {
    const auto priority = dirtyMeshPriority(coord);
    if (!priority) {
        return false;
    }

    if (prioritize) {
        m_priorityMeshRequests.insert(coord);
    }
    const bool prioritized =
        m_priorityMeshRequests.find(coord) != m_priorityMeshRequests.end();
    m_configRetiredWork.erase(coord);
    m_meshDependencyWaiting.erase(coord);
    m_loadGenQueued.erase(coord);

    auto flightIt = m_meshInFlight.find(coord);
    if (flightIt != m_meshInFlight.end()) {
        if (flightIt->second.obsolete && hasEligibleMeshWork(coord)) {
            setReplacementPending(coord, flightIt->second, true);
            flightIt->second.prioritized =
                flightIt->second.prioritized || prioritized;
        } else if (flightIt->second.obsolete) {
            setReplacementPending(coord, flightIt->second, false);
            flightIt->second.prioritized = false;
        }
        m_priorityMeshRequests.erase(coord);
        erasePendingMesh(coord);
        return false;
    }

    auto pendingIt = m_pendingMeshes.find(coord);
    if (pendingIt != m_pendingMeshes.end()) {
        PendingMeshRequest& pending = pendingIt->second;
        if (pending.priority == *priority &&
            pending.kind == kind &&
            pending.prioritized == prioritized) {
            return false;
        }
        pending.priority = *priority;
        pending.kind = kind;
        pending.prioritized = prioritized;
        pending.sequence = m_nextPendingMeshSequence++;
        if (m_nextPendingMeshSequence == 0) {
            m_nextPendingMeshSequence = 1;
        }
        m_pendingMeshQueues[static_cast<size_t>(kind)].push(pending);
        compactPendingMeshQueuesIfNeeded();
        markVisibilityMeshEligible(coord, false);
        return false;
    }

    PendingMeshRequest pending{
        .priority = *priority,
        .coord = coord,
        .kind = kind,
        .prioritized = prioritized,
        .sequence = m_nextPendingMeshSequence++
    };
    if (m_nextPendingMeshSequence == 0) {
        m_nextPendingMeshSequence = 1;
    }
    m_pendingMeshes.emplace(coord, pending);
    m_pendingMeshQueues[static_cast<size_t>(kind)].push(pending);
    compactPendingMeshQueuesIfNeeded();
    markVisibilityMeshEligible(coord, false);
    return true;
}

void ChunkStreamer::compactPendingMeshQueuesIfNeeded() {
    const size_t queueRecords =
        m_pendingMeshQueues[static_cast<size_t>(MeshRequestKind::Missing)].size() +
        m_pendingMeshQueues[static_cast<size_t>(MeshRequestKind::Dirty)].size();
    if (m_pendingMeshes.empty()) {
        if (queueRecords > 0) {
            m_pendingMeshQueues = {};
        }
        return;
    }
    if (queueRecords <= m_pendingMeshes.size() * 2 + 8) {
        return;
    }

    decltype(m_pendingMeshQueues) compacted;
    for (const auto& entry : m_pendingMeshes) {
        ++m_workMetrics.schedulerCoordinatesInspected;
        const PendingMeshRequest& request = entry.second;
        compacted[static_cast<size_t>(request.kind)].push(request);
    }
    m_pendingMeshQueues = std::move(compacted);
}

void ChunkStreamer::erasePendingMesh(ChunkCoord coord) {
    m_pendingMeshes.erase(coord);
    compactPendingMeshQueuesIfNeeded();
}

void ChunkStreamer::retirePendingMesh(ChunkCoord coord) {
    erasePendingMesh(coord);
    m_meshDependencyWaiting.erase(coord);
}

void ChunkStreamer::rememberConfigRetiredWork(
    ChunkCoord coord,
    ConfigRetiredWorkKind kind) {
    if (m_chunkManager && m_chunkManager->getChunk(coord)) {
        m_streamedResidents.insert(coord);
    }
    if (kind == ConfigRetiredWorkKind::DirtyMesh) {
        Chunk* chunk = m_chunkManager
            ? m_chunkManager->getChunk(coord)
            : nullptr;
        if (chunk && !chunk->isEmpty() && m_generator &&
            chunk->worldGenVersion() == m_generator->semanticsVersion()) {
            chunk->markDirty();
        }
    }
    auto [it, inserted] = m_configRetiredWork.emplace(coord, kind);
    if (inserted) {
        return;
    }
    auto rank = [](ConfigRetiredWorkKind value) {
        switch (value) {
            case ConfigRetiredWorkKind::DirtyMesh:
                return 2;
            case ConfigRetiredWorkKind::MissingMesh:
                return 1;
            case ConfigRetiredWorkKind::LoadGen:
                return 0;
        }
        return 0;
    };
    if (rank(kind) > rank(it->second)) {
        it->second = kind;
    }
}

bool ChunkStreamer::isConfigRetiredMeshEligible(
    ChunkCoord coord,
    ConfigRetiredWorkKind kind) const {
    return kind != ConfigRetiredWorkKind::LoadGen &&
        hasMeshReconciliationWork(coord);
}

bool ChunkStreamer::hasMeshReconciliationWork(ChunkCoord coord) const {
    if (!dirtyMeshPriority(coord)) {
        return false;
    }
    const Chunk* chunk = m_chunkManager
        ? m_chunkManager->getChunk(coord)
        : nullptr;
    if (!chunk || !m_generator ||
        chunk->worldGenVersion() != m_generator->semanticsVersion()) {
        return false;
    }
    if (chunk->isEmpty()) {
        return m_meshStore && m_meshStore->contains(coord);
    }
    return hasEligibleMeshWork(coord);
}

ChunkStreamer::PendingWorkKind ChunkStreamer::classifyPendingWork(
    ChunkCoord coord) const {
    const Chunk* chunk = m_chunkManager
        ? m_chunkManager->getChunk(coord)
        : nullptr;
    if (!chunk || !m_generator ||
        chunk->worldGenVersion() != m_generator->semanticsVersion()) {
        return PendingWorkKind::Generation;
    }
    if (chunk->isEmpty()) {
        return m_meshStore && m_meshStore->contains(coord)
            ? PendingWorkKind::Mesh
            : PendingWorkKind::None;
    }

    ChunkState state = ChunkState::Missing;
    auto stateIt = m_states.find(coord);
    if (stateIt != m_states.end()) {
        state = stateIt->second;
    }
    const bool hasMesh = m_meshStore && m_meshStore->contains(coord);
    const bool isMeshed = hasMesh || state == ChunkState::ReadyMesh ||
        m_worldBoundsSuppressedMeshes.find(coord) !=
            m_worldBoundsSuppressedMeshes.end();
    return !isMeshed || chunk->isDirty()
        ? PendingWorkKind::Mesh
        : PendingWorkKind::None;
}

bool ChunkStreamer::hasCanonicalWorkOwner(
    ChunkCoord coord,
    bool includeLoadGenQueue) const {
    if ((includeLoadGenQueue &&
         m_loadGenQueued.find(coord) != m_loadGenQueued.end()) ||
        m_pendingGenerations.find(coord) !=
            m_pendingGenerations.end() ||
        m_loadPending.find(coord) != m_loadPending.end() ||
        m_generationFlights.find(coord) != m_generationFlights.end() ||
        m_pendingMeshes.find(coord) != m_pendingMeshes.end() ||
        m_meshDependencyWaiting.find(coord) !=
            m_meshDependencyWaiting.end() ||
        m_evictionRetryAfter.find(coord) != m_evictionRetryAfter.end() ||
        m_versionReplacementWaiting.find(coord) !=
            m_versionReplacementWaiting.end()) {
        return true;
    }
    auto flightIt = m_meshInFlight.find(coord);
    return flightIt != m_meshInFlight.end() &&
        (!flightIt->second.obsolete ||
         flightIt->second.replacementPending);
}

void ChunkStreamer::reconcileConfigRetiredWork(
    uint64_t& schedulerCoordinatesInspected) {
    using RetiredEntry =
        std::pair<ChunkCoord, ConfigRetiredWorkKind>;
    std::vector<RetiredEntry> retired;
    retired.reserve(m_configRetiredWork.size());
    for (const auto& entry : m_configRetiredWork) {
        ++schedulerCoordinatesInspected;
        retired.push_back(entry);
    }
    std::sort(
        retired.begin(),
        retired.end(),
        [&](const RetiredEntry& lhs, const RetiredEntry& rhs) {
            const auto lhsPriority = m_desiredPriority.find(lhs.first);
            const auto rhsPriority = m_desiredPriority.find(rhs.first);
            const bool lhsDirect = lhsPriority != m_desiredPriority.end() &&
                hasDirectStreamingDemand(lhs.first);
            const bool rhsDirect = rhsPriority != m_desiredPriority.end() &&
                hasDirectStreamingDemand(rhs.first);
            if (lhsDirect != rhsDirect) {
                return lhsDirect;
            }
            if (lhsDirect && lhsPriority->second != rhsPriority->second) {
                return lhsPriority->second < rhsPriority->second;
            }
            if (m_lastCenter) {
                const int lhsDistance =
                    distanceSquared(*m_lastCenter, lhs.first);
                const int rhsDistance =
                    distanceSquared(*m_lastCenter, rhs.first);
                if (lhsDistance != rhsDistance) {
                    return lhsDistance < rhsDistance;
                }
            }
            return lhs.first < rhs.first;
        });

    for (const auto& [coord, kind] : retired) {
        const bool directlyDesired = hasDirectStreamingDemand(coord);
        const bool meshEligible =
            isConfigRetiredMeshEligible(coord, kind);

        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end() &&
            stateIt->second == ChunkState::QueuedGen &&
            m_generationFlights.find(coord) == m_generationFlights.end()) {
            m_states.erase(stateIt);
            stateIt = m_states.end();
        }
        if (stateIt != m_states.end() &&
            stateIt->second == ChunkState::QueuedMesh) {
            auto flightIt = m_meshInFlight.find(coord);
            const bool hasMeshOwner =
                m_pendingMeshes.find(coord) != m_pendingMeshes.end() ||
                m_meshDependencyWaiting.find(coord) !=
                    m_meshDependencyWaiting.end() ||
                (flightIt != m_meshInFlight.end() &&
                 (!flightIt->second.obsolete ||
                  flightIt->second.replacementPending));
            if (!hasMeshOwner) {
                Chunk* chunk = m_chunkManager->getChunk(coord);
                if (!chunk) {
                    m_states.erase(stateIt);
                } else if (m_meshStore->contains(coord)) {
                    stateIt->second = ChunkState::ReadyMesh;
                } else {
                    stateIt->second = ChunkState::ReadyData;
                }
            }
        }

        if (!directlyDesired && !meshEligible) {
            Chunk* chunk = m_chunkManager
                ? m_chunkManager->getChunk(coord)
                : nullptr;
            const int unloadDistance = std::max(
                std::max(0, m_config.viewDistanceChunks),
                m_config.unloadDistanceChunks);
            const bool outsideUnloadRadius = m_lastCenter &&
                distanceSquared(*m_lastCenter, coord) >
                    unloadDistance * unloadDistance;
            const bool cachePressure = m_cache.maxChunks() > 0 &&
                m_cache.size() > m_cache.maxChunks() &&
                m_desiredSet.find(coord) == m_desiredSet.end();
            if (kind != ConfigRetiredWorkKind::LoadGen &&
                chunk && chunk->isPersistDirty() &&
                (outsideUnloadRadius || cachePressure)) {
                continue;
            }
            m_configRetiredWork.erase(coord);
            continue;
        }

        if (kind == ConfigRetiredWorkKind::LoadGen) {
            queueLoadGen(coord);
        } else {
            Chunk* chunk = m_chunkManager->getChunk(coord);
            const bool currentVersion = chunk && m_generator &&
                chunk->worldGenVersion() ==
                    m_generator->semanticsVersion();
            if (!currentVersion) {
                if (directlyDesired) {
                    queueLoadGen(coord);
                } else {
                    continue;
                }
            } else {
                if (kind == ConfigRetiredWorkKind::DirtyMesh) {
                    chunk->markDirty();
                }
                queueDirtyMesh(coord);
            }
        }
        m_configRetiredWork.erase(coord);
    }
}

bool ChunkStreamer::hasEligibleMeshWork(ChunkCoord coord) const {
    if (!dirtyMeshPriority(coord)) {
        return false;
    }
    Chunk* chunk = m_chunkManager
        ? m_chunkManager->getChunk(coord)
        : nullptr;
    if (!chunk || !m_generator || chunk->isEmpty() ||
        chunk->worldGenVersion() != m_generator->semanticsVersion()) {
        return false;
    }

    ChunkState state = ChunkState::Missing;
    auto stateIt = m_states.find(coord);
    if (stateIt != m_states.end()) {
        state = stateIt->second;
    }
    const bool hasMesh = m_meshStore && m_meshStore->contains(coord);
    const bool isMeshed = hasMesh || state == ChunkState::ReadyMesh ||
        m_worldBoundsSuppressedMeshes.find(coord) !=
            m_worldBoundsSuppressedMeshes.end();
    return !isMeshed || chunk->isDirty();
}

void ChunkStreamer::setReplacementPending(ChunkCoord coord,
                                          MeshInFlight& flight,
                                          bool pending) {
    if (flight.replacementPending == pending) {
        return;
    }
    flight.replacementPending = pending;
    if (pending) {
        ++m_replacementPendingMeshCount;
        m_configRetiredWork.erase(coord);
        retirePendingMesh(coord);
        m_loadGenQueued.erase(coord);
        erasePendingGeneration(coord);
        cancelPendingLoad(coord);
    } else {
        assert(m_replacementPendingMeshCount > 0);
        --m_replacementPendingMeshCount;
    }
}

void ChunkStreamer::retireReplacementPending(ChunkCoord coord) {
    auto flightIt = m_meshInFlight.find(coord);
    if (flightIt == m_meshInFlight.end()) {
        return;
    }
    setReplacementPending(coord, flightIt->second, false);
    flightIt->second.prioritized = false;
}

void ChunkStreamer::queueDirtyMesh(ChunkCoord coord, bool prioritize) {
    if (m_chunkManager && m_chunkManager->getChunk(coord)) {
        m_streamedResidents.insert(coord);
    }
    const auto priority = dirtyMeshPriority(coord);
    if (!priority) {
        Chunk* chunk = m_chunkManager
            ? m_chunkManager->getChunk(coord)
            : nullptr;
        const bool currentVersion = chunk &&
            (!m_generator ||
             chunk->worldGenVersion() ==
                 m_generator->semanticsVersion());
        if (currentVersion && chunk->isEmpty()) {
            m_worldBoundsSuppressedMeshes.erase(coord);
        }
        if (m_configRetiredWork.find(coord) !=
                m_configRetiredWork.end() ||
            m_evictionRetryAfter.find(coord) !=
                m_evictionRetryAfter.end()) {
            retirePendingMesh(coord);
            retireReplacementPending(coord);
            m_priorityMeshRequests.erase(coord);
            return;
        }
        if (currentVersion && chunk->isEmpty()) {
            if (m_meshStore) {
                m_meshStore->remove(coord);
            }
            chunk->clearDirty();
            m_countedMeshRetryRevisions.erase(coord);
            eraseFailure(m_meshErrors, m_meshFailureVersion, coord);
            m_states[coord] = ChunkState::ReadyMesh;
            queueLoadedNeighbors(coord);
            completePendingVisibilityTrace(
                coord, ChunkVisibilityOutcome::VoxelEmpty);
        }
        retirePendingMesh(coord);
        retireReplacementPending(coord);
        m_priorityMeshRequests.erase(coord);
        m_configRetiredWork.erase(coord);
        return;
    }
    const bool wasDependencyWaiting =
        m_meshDependencyWaiting.find(coord) !=
        m_meshDependencyWaiting.end();
    if (prioritize) {
        m_priorityMeshRequests.insert(coord);
    }
    if (m_visibilityTracer && m_visibilityTracer->traces(coord)) {
        Chunk* chunk = m_chunkManager
            ? m_chunkManager->getChunk(coord)
            : nullptr;
        const bool hasMesh = m_meshStore && m_meshStore->contains(coord);
        ChunkState state = ChunkState::Missing;
        auto stateIt = m_states.find(coord);
        if (stateIt != m_states.end()) {
            state = stateIt->second;
        }
        const bool isMeshed = hasMesh || state == ChunkState::ReadyMesh ||
            m_worldBoundsSuppressedMeshes.find(coord) !=
                m_worldBoundsSuppressedMeshes.end();
        const bool meshInFlight =
            m_meshInFlight.find(coord) != m_meshInFlight.end();
        const bool dirty = chunk && chunk->isDirty();
        const bool replacementNeeded =
            (!isMeshed || dirty) && (!meshInFlight || dirty);
        if (replacementNeeded) {
            const auto lifecycleKind = isMeshed
                ? ChunkVisibilityLifecycleKind::Remesh
                : ChunkVisibilityLifecycleKind::CameraDemand;
            ensureVisibilityTrace(
                coord,
                lifecycleKind,
                lifecycleKind == ChunkVisibilityLifecycleKind::Remesh
                    ? ChunkVisibilityOrigin::Remesh
                    : ChunkVisibilityOrigin::ResidentLeftCensored);
        }
    }

    Chunk* chunk = m_chunkManager
        ? m_chunkManager->getChunk(coord)
        : nullptr;
    if (!chunk) {
        retirePendingMesh(coord);
        markVisibilityMeshEligible(coord, false);
        if (m_desiredSet.find(coord) != m_desiredSet.end()) {
            queueLoadGen(coord);
        } else {
            m_priorityMeshRequests.erase(coord);
            m_configRetiredWork.erase(coord);
        }
        return;
    }
    if (m_generator &&
        chunk->worldGenVersion() != m_generator->semanticsVersion()) {
        retirePendingMesh(coord);
        retireReplacementPending(coord);
        m_priorityMeshRequests.erase(coord);
        if (m_desiredSet.find(coord) != m_desiredSet.end()) {
            queueLoadGen(coord);
        } else {
            m_configRetiredWork.erase(coord);
        }
        return;
    }
    if (chunk->isEmpty()) {
        if (m_meshStore) {
            m_meshStore->remove(coord);
        }
        m_worldBoundsSuppressedMeshes.erase(coord);
        chunk->clearDirty();
        retirePendingMesh(coord);
        retireReplacementPending(coord);
        m_priorityMeshRequests.erase(coord);
        m_countedMeshRetryRevisions.erase(coord);
        eraseFailure(m_meshErrors, m_meshFailureVersion, coord);
        m_states[coord] = ChunkState::ReadyMesh;
        queueLoadedNeighbors(coord);
        completePendingVisibilityTrace(
            coord, ChunkVisibilityOutcome::VoxelEmpty);
        m_configRetiredWork.erase(coord);
        return;
    }

    auto flightIt = m_meshInFlight.find(coord);
    const bool prioritized =
        m_priorityMeshRequests.find(coord) != m_priorityMeshRequests.end();
    if (flightIt != m_meshInFlight.end() && !flightIt->second.obsolete) {
        if (chunk && chunk->isDirty() &&
            flightIt->second.observedRevision != chunk->meshRevision()) {
            flightIt->second.observedRevision = chunk->meshRevision();
            ++m_workMetrics.meshInvalidations;
            ++m_workMetrics.meshRequestsCoalesced;
        }
        flightIt->second.prioritized =
            flightIt->second.prioritized || prioritized;
        m_priorityMeshRequests.erase(coord);
        m_configRetiredWork.erase(coord);
        return;
    }

    ChunkState state = ChunkState::Missing;
    auto stateIt = m_states.find(coord);
    if (stateIt != m_states.end()) {
        state = stateIt->second;
    }
    const bool hasMesh = m_meshStore && m_meshStore->contains(coord);
    const bool isMeshed = hasMesh || state == ChunkState::ReadyMesh ||
        m_worldBoundsSuppressedMeshes.find(coord) !=
            m_worldBoundsSuppressedMeshes.end();
    if (isMeshed && !chunk->isDirty()) {
        m_priorityMeshRequests.erase(coord);
        retirePendingMesh(coord);
        retireReplacementPending(coord);
        m_configRetiredWork.erase(coord);
        return;
    }
    if (!isMeshed && !prioritized && !wasDependencyWaiting) {
        queueLoadGen(coord);
        return;
    }
    if (!hasAllNeighborsLoaded(coord)) {
        waitForMeshDependencies(coord);
        markVisibilityMeshEligible(coord, false);
        return;
    }

    queuePendingMesh(
        coord,
        isMeshed ? MeshRequestKind::Dirty : MeshRequestKind::Missing,
        prioritized);
}

void ChunkStreamer::ensureVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityLifecycleKind kind,
    ChunkVisibilityOrigin origin) {
    if (!m_visibilityTracer || !m_visibilityTracer->traces(coord)) {
        return;
    }
    auto& slot = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (slot && slot->key.coord == coord) {
        return;
    }

    PendingVisibilityTrace pending;
    pending.kind = kind;
    pending.tracer = m_visibilityTracer;
    const auto key = pending.tracer->begin(pending.kind, origin);
    if (!key) {
        return;
    }
    pending.key = *key;
    slot = std::move(pending);
}

void ChunkStreamer::beginCameraVisibilityTrace(ChunkCoord coord) {
    const auto kind = ChunkVisibilityLifecycleKind::CameraDemand;
    if (!m_visibilityTracer || !m_visibilityTracer->traces(coord)) {
        return;
    }
    ensureVisibilityTrace(
        coord,
        kind,
        m_chunkManager && m_chunkManager->getChunk(coord)
            ? ChunkVisibilityOrigin::ResidentLeftCensored
            : ChunkVisibilityOrigin::Unresolved);
    markVisibilityStage(coord, ChunkVisibilityStage::Desired);
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord || !m_meshStore) {
        return;
    }

    const ChunkVisibilityTraceLink link{
        pending->key,
        pending->kind,
        pending->tracer
    };
    const auto attachment =
        m_meshStore->attachCachedVisibilityTrace(coord, link);
    if (attachment == CachedMeshTraceAttachment::Missing) {
        return;
    }
    completePendingVisibilityTrace(
        coord,
        kind,
        attachment == CachedMeshTraceAttachment::EmptyGeometry
            ? ChunkVisibilityOutcome::CachedEmptyGeometry
            : ChunkVisibilityOutcome::CachedNonemptyGeometry);
}

void ChunkStreamer::observeVisibilityDataReady(
    ChunkCoord coord,
    ChunkVisibilityOrigin origin) {
    const auto kind = ChunkVisibilityLifecycleKind::CameraDemand;
    ensureVisibilityTrace(coord, kind);
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return;
    }
    pending->tracer->markDataReady(pending->key, origin);
}

bool ChunkStreamer::areFaceNeighbors(ChunkCoord lhs, ChunkCoord rhs) {
    const int64_t dx = std::abs(
        static_cast<int64_t>(lhs.x) - static_cast<int64_t>(rhs.x));
    const int64_t dy = std::abs(
        static_cast<int64_t>(lhs.y) - static_cast<int64_t>(rhs.y));
    const int64_t dz = std::abs(
        static_cast<int64_t>(lhs.z) - static_cast<int64_t>(rhs.z));
    return dx + dy + dz == 1;
}

void ChunkStreamer::observeVisibilityNeighborReadiness(ChunkCoord coord) {
    if (!m_visibilityTracer) {
        return;
    }
    const ChunkCoord traced = m_visibilityTracer->coord();
    if (coord == traced) {
        markVisibilityMeshEligible(traced, false);
    } else if (areFaceNeighbors(traced, coord) &&
               m_desiredSet.find(coord) != m_desiredSet.end()) {
        markVisibilityMeshEligible(traced, true);
    }
}

void ChunkStreamer::refreshVisibilityDependencySnapshot() {
    if (!m_visibilityTracer || !m_chunkManager) {
        return;
    }
    const ChunkCoord traced = m_visibilityTracer->coord();
    if (!m_chunkManager->getChunk(traced)) {
        return;
    }
    const bool hasPendingTrace = std::any_of(
        m_pendingVisibilityTraces.begin(),
        m_pendingVisibilityTraces.end(),
        [&](const auto& pending) {
            return pending && pending->key.coord == traced;
        });
    if (!hasPendingTrace) {
        return;
    }

    const uint8_t missingNeighbors =
        countMissingDesiredCardinalNeighbors(traced);
    const auto blockers = visibilityDependencySnapshot(traced);
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (!pending || pending->key.coord != traced) {
            continue;
        }
        pending->tracer->observeMissingDesiredCardinalNeighborCount(
            pending->key, missingNeighbors);
        pending->tracer->observeBlockingDesiredCardinalNeighbors(
            pending->key, blockers);
    }
}

ChunkVisibilityBlockingNeighborSnapshot
ChunkStreamer::visibilityDependencySnapshot(ChunkCoord coord) const {
    ChunkVisibilityBlockingNeighborSnapshot blockers;
    for (int index = 0; index < DirectionCount; ++index) {
        const Direction direction = static_cast<Direction>(index);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(direction, dx, dy, dz);
        const ChunkCoord neighbor = coord.offset(dx, dy, dz);
        const bool required =
            m_desiredSet.find(neighbor) != m_desiredSet.end();
        blockers.neighbors[static_cast<size_t>(index)] = {
            direction,
            neighbor,
            required,
            classifyVisibilityBlocker(neighbor)};
    }
    blockers.count = static_cast<uint8_t>(DirectionCount);
    return blockers;
}

void ChunkStreamer::markVisibilityMeshEligible(
    ChunkCoord coord,
    bool neighborBecameReady) {
    if (!m_visibilityTracer || !m_visibilityTracer->traces(coord) ||
        !m_chunkManager || !m_meshStore) {
        return;
    }

    Chunk* chunk = m_chunkManager->getChunk(coord);
    if (!chunk) {
        return;
    }

    ChunkState state = ChunkState::Missing;
    auto stateIt = m_states.find(coord);
    if (stateIt != m_states.end()) {
        state = stateIt->second;
    }
    const bool isMeshed =
        m_meshStore->contains(coord) || state == ChunkState::ReadyMesh;
    const bool initialMeshRequested = !isMeshed &&
        (m_desiredSet.find(coord) != m_desiredSet.end() ||
         m_priorityMeshRequests.find(coord) !=
             m_priorityMeshRequests.end());
    const bool meshInFlight =
        m_meshInFlight.find(coord) != m_meshInFlight.end();
    const bool remeshRequested = chunk->isDirty() &&
        (isMeshed || meshInFlight);
    if ((!initialMeshRequested && !remeshRequested) || chunk->isEmpty()) {
        return;
    }
    if (initialMeshRequested &&
        m_loadPending.find(coord) != m_loadPending.end()) {
        return;
    }
    if (meshInFlight && !neighborBecameReady) {
        return;
    }

    const auto kind = remeshRequested
        ? ChunkVisibilityLifecycleKind::Remesh
        : ChunkVisibilityLifecycleKind::CameraDemand;
    if (neighborBecameReady) {
        ensureVisibilityTrace(
            coord,
            kind,
            kind == ChunkVisibilityLifecycleKind::Remesh
                ? ChunkVisibilityOrigin::Remesh
                : ChunkVisibilityOrigin::ResidentLeftCensored);
    }
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return;
    }

    const uint8_t missingNeighbors =
        countMissingDesiredCardinalNeighbors(coord);
    pending->tracer->observeMissingDesiredCardinalNeighborCount(
        pending->key, missingNeighbors);
    const auto blockers = visibilityDependencySnapshot(coord);
    pending->tracer->observeBlockingDesiredCardinalNeighbors(
        pending->key, blockers);
    if (missingNeighbors != 0) {
        return;
    }

    if (neighborBecameReady && meshInFlight) {
        pending->tracer->mark(
            pending->key,
            ChunkVisibilityStage::NeighborReady);
    } else if (neighborBecameReady) {
        pending->tracer->mark(
            pending->key,
            {
                ChunkVisibilityStage::NeighborReady,
                ChunkVisibilityStage::MeshEligible,
                ChunkVisibilityStage::SchedulerWait
            });
    } else if (!meshInFlight) {
        pending->tracer->mark(
            pending->key,
            {
                ChunkVisibilityStage::MeshEligible,
                ChunkVisibilityStage::SchedulerWait
            });
    }
}

void ChunkStreamer::markVisibilityStage(ChunkCoord coord,
                                        ChunkVisibilityStage stage) {
    const auto kind = ChunkVisibilityLifecycleKind::CameraDemand;
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if ((!pending || pending->key.coord != coord) &&
        stage == ChunkVisibilityStage::DataRequest) {
        ensureVisibilityTrace(coord, kind);
    }
    if (!pending || pending->key.coord != coord ||
        stage == ChunkVisibilityStage::Count) {
        return;
    }
    pending->tracer->mark(pending->key, stage);
}

std::optional<ChunkVisibilityTraceLink>
ChunkStreamer::currentVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityLifecycleKind kind) const {
    const auto& pending =
        m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return std::nullopt;
    }
    return ChunkVisibilityTraceLink{
        pending->key,
        pending->kind,
        pending->tracer
    };
}

ChunkVisibilityBlockerState ChunkStreamer::classifyVisibilityBlocker(
    ChunkCoord coord) const {
    if (m_desiredSet.find(coord) == m_desiredSet.end()) {
        return ChunkVisibilityBlockerState::NoLongerRequired;
    }
    if (m_chunkManager && m_chunkManager->getChunk(coord)) {
        return ChunkVisibilityBlockerState::ReadyResident;
    }
    if (m_loadPending.find(coord) != m_loadPending.end()) {
        const auto state = m_chunkLoadExecutionState
            ? m_chunkLoadExecutionState(coord)
            : std::nullopt;
        if (!state) {
            return ChunkVisibilityBlockerState::LoadRequestPending;
        }
        if (state->owner == ChunkLoadExecutionOwner::Payload) {
            switch (state->phase) {
                case ChunkLoadExecutionPhase::SchedulerPending:
                    return ChunkVisibilityBlockerState::
                        LoadPayloadSchedulerPending;
                case ChunkLoadExecutionPhase::PoolQueued:
                    return ChunkVisibilityBlockerState::LoadPayloadPoolQueued;
                case ChunkLoadExecutionPhase::WorkerRunning:
                    return ChunkVisibilityBlockerState::
                        LoadPayloadWorkerRunning;
                case ChunkLoadExecutionPhase::ResultPublished:
                    return ChunkVisibilityBlockerState::
                        LoadPayloadResultPublished;
                case ChunkLoadExecutionPhase::RetryWaiting:
                    return ChunkVisibilityBlockerState::Unowned;
                case ChunkLoadExecutionPhase::TerminalFailed:
                    return ChunkVisibilityBlockerState::
                        LoadPayloadTerminalFailed;
            }
        }
        switch (state->phase) {
            case ChunkLoadExecutionPhase::SchedulerPending:
                return ChunkVisibilityBlockerState::
                    LoadRegionSchedulerPending;
            case ChunkLoadExecutionPhase::PoolQueued:
                return ChunkVisibilityBlockerState::LoadRegionPoolQueued;
            case ChunkLoadExecutionPhase::WorkerRunning:
                return ChunkVisibilityBlockerState::LoadRegionWorkerRunning;
            case ChunkLoadExecutionPhase::ResultPublished:
                return ChunkVisibilityBlockerState::
                    LoadRegionResultPublished;
            case ChunkLoadExecutionPhase::RetryWaiting:
                return ChunkVisibilityBlockerState::LoadRegionRetryWaiting;
            case ChunkLoadExecutionPhase::TerminalFailed:
                return ChunkVisibilityBlockerState::LoadRegionTerminalFailed;
        }
    }
    if (m_loadErrors.find(coord) != m_loadErrors.end()) {
        return ChunkVisibilityBlockerState::LoadTerminalFailed;
    }
    const auto flight = m_generationFlights.find(coord);
    if (flight != m_generationFlights.end()) {
        switch (flight->second->phase.load(std::memory_order_acquire)) {
            case GenerationExecutorPhase::Submitting:
                return ChunkVisibilityBlockerState::
                    GenerationSchedulerPending;
            case GenerationExecutorPhase::ExecutorQueued:
                return ChunkVisibilityBlockerState::GenerationExecutorQueued;
            case GenerationExecutorPhase::WorkerRunning:
                return ChunkVisibilityBlockerState::GenerationWorkerRunning;
            case GenerationExecutorPhase::ResultPublished:
                return ChunkVisibilityBlockerState::
                    GenerationResultPublished;
        }
    }
    if (m_pendingGenerations.find(coord) != m_pendingGenerations.end()) {
        return m_inFlightGen >= generationDispatchLimit()
            ? ChunkVisibilityBlockerState::GenerationCapacityWaiting
            : ChunkVisibilityBlockerState::GenerationSchedulerPending;
    }
    if (m_generationErrors.find(coord) != m_generationErrors.end()) {
        return ChunkVisibilityBlockerState::GenerationTerminalFailed;
    }
    if (m_loadGenQueued.find(coord) != m_loadGenQueued.end()) {
        return ChunkVisibilityBlockerState::SourceResolutionPending;
    }
    return ChunkVisibilityBlockerState::Unowned;
}

std::optional<ChunkVisibilityTraceLink>
ChunkStreamer::bindVisibilityTrace(
    ChunkCoord coord,
    const ChunkVisibilityMeshTaskIdentity& meshTask,
    ChunkVisibilityLifecycleKind kind) {
    ensureVisibilityTrace(
        coord,
        kind,
        kind == ChunkVisibilityLifecycleKind::Remesh
            ? ChunkVisibilityOrigin::Remesh
            : ChunkVisibilityOrigin::ResidentLeftCensored);
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return std::nullopt;
    }

    pending->tracer->bindMeshTask(pending->key, meshTask);
    const ChunkVisibilityTraceLink link{
        pending->key,
        pending->kind,
        pending->tracer
    };
    pending.reset();
    return link;
}

void ChunkStreamer::completePendingVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityOutcome outcome) {
    for (const auto kind : {
             ChunkVisibilityLifecycleKind::CameraDemand,
             ChunkVisibilityLifecycleKind::Remesh}) {
        completePendingVisibilityTrace(coord, kind, outcome);
    }
}

void ChunkStreamer::completePendingVisibilityTrace(
    ChunkCoord coord,
    ChunkVisibilityLifecycleKind kind,
    ChunkVisibilityOutcome outcome) {
    auto& pending = m_pendingVisibilityTraces[visibilityKindIndex(kind)];
    if (!pending || pending->key.coord != coord) {
        return;
    }
    pending->tracer->complete(pending->key, outcome);
    pending.reset();
}

void ChunkStreamer::completeInFlightVisibilityTrace(
    MeshInFlight& flight,
    ChunkVisibilityOutcome outcome) {
    if (flight.visibilityTracer && flight.visibilityTrace) {
        flight.visibilityTracer->complete(*flight.visibilityTrace, outcome);
        flight.visibilityTrace.reset();
        flight.visibilityTracer.reset();
    }
}

void ChunkStreamer::abandonVisibilityTraces(
    ChunkVisibilityOutcome outcome) {
    for (const auto& pending : m_pendingVisibilityTraces) {
        if (pending) {
            completePendingVisibilityTrace(
                pending->key.coord,
                outcome);
            break;
        }
    }

    for (auto& [coord, flight] : m_meshInFlight) {
        completeInFlightVisibilityTrace(flight, outcome);
    }

    MeshResult meshResult;
    while (m_meshComplete.tryPop(meshResult)) {
        if (meshResult.visibilityTracer && meshResult.visibilityTrace) {
            meshResult.visibilityTracer->complete(
                *meshResult.visibilityTrace,
                outcome);
        }
    }
}

void ChunkStreamer::reprioritizePendingGenerations(
    uint64_t& schedulerCoordinatesInspected) {
    if (!m_lastCenter || m_pendingGenerations.empty()) {
        return;
    }

    decltype(m_pendingGenerationQueue) reprioritized;
    for (auto it = m_pendingGenerations.begin();
         it != m_pendingGenerations.end();) {
        ++schedulerCoordinatesInspected;
        if (!hasDirectStreamingDemand(it->first)) {
            it = m_pendingGenerations.erase(it);
            continue;
        }
        it->second = chunkImportance(*m_lastCenter, it->first);
        if (m_generationFlights.find(it->first) ==
            m_generationFlights.end()) {
            reprioritized.insert(it->second);
        }
        ++it;
    }
    m_pendingGenerationQueue = std::move(reprioritized);
}

size_t ChunkStreamer::generationDispatchLimit() const {
    size_t limit = m_config.genQueueLimit > 0
        ? m_config.genQueueLimit
        : std::numeric_limits<size_t>::max();
    const size_t workerCount = m_genPool
        ? m_genPool->threadCount()
        : 0;
    if (workerCount > 0) {
        // Keep one worker-width standby wave queued so workers can continue
        // without waiting for the owner-thread completion drain.
        limit = std::min(limit, workerCount * 2);
    } else {
        // Inline jobs complete during dispatch, but remain owned until the
        // main-thread drain observes the single executor slot.
        limit = std::min(limit, static_cast<size_t>(1));
    }
    return limit;
}

void ChunkStreamer::dispatchPendingGenerations(
    uint64_t& schedulerCoordinatesInspected) {
    const size_t dispatchLimit = generationDispatchLimit();
    if (m_inFlightGen >= dispatchLimit ||
        m_pendingGenerationQueue.empty()) {
        return;
    }

    PROFILE_SCOPE("Streaming/Update/GenerationDispatch");
    while (m_inFlightGen < dispatchLimit &&
           !m_pendingGenerationQueue.empty()) {
        const ChunkImportance importance =
            *m_pendingGenerationQueue.begin();
        m_pendingGenerationQueue.erase(m_pendingGenerationQueue.begin());
        ++schedulerCoordinatesInspected;

        auto pendingIt = m_pendingGenerations.find(importance.coord);
        if (pendingIt == m_pendingGenerations.end() ||
            pendingIt->second != importance) {
            continue;
        }
        if (m_generationFlights.find(importance.coord) !=
            m_generationFlights.end()) {
            continue;
        }
        m_pendingGenerations.erase(pendingIt);
        if (!hasDirectStreamingDemand(importance.coord)) {
            continue;
        }
        enqueueGeneration(importance.coord);
    }

    if (m_inFlightGen < dispatchLimit) {
        return;
    }
    for (const auto& pendingTrace : m_pendingVisibilityTraces) {
        if (pendingTrace &&
            m_pendingGenerations.find(pendingTrace->key.coord) !=
                m_pendingGenerations.end()) {
            markVisibilityStage(
                pendingTrace->key.coord,
                ChunkVisibilityStage::GenerationCapacityWait);
        }
    }
}

void ChunkStreamer::reprioritizePendingMeshes(
    uint64_t& schedulerCoordinatesInspected) {
    decltype(m_pendingMeshQueues) prioritized;
    for (auto it = m_priorityMeshRequests.begin();
         it != m_priorityMeshRequests.end(); ) {
        ++schedulerCoordinatesInspected;
        if (!dirtyMeshPriority(*it)) {
            it = m_priorityMeshRequests.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_pendingMeshes.begin(); it != m_pendingMeshes.end(); ) {
        ++schedulerCoordinatesInspected;
        const auto priority = dirtyMeshPriority(it->first);
        if (!priority) {
            it = m_pendingMeshes.erase(it);
            continue;
        }
        PendingMeshRequest& request = it->second;
        request.priority = *priority;
        request.prioritized = m_priorityMeshRequests.find(it->first) !=
            m_priorityMeshRequests.end();
        prioritized[static_cast<size_t>(request.kind)].push(request);
        ++it;
    }
    m_pendingMeshQueues = std::move(prioritized);
}

size_t ChunkStreamer::meshDispatchLimit() const {
    size_t limit = m_config.meshQueueLimit > 0
        ? m_config.meshQueueLimit
        : std::numeric_limits<size_t>::max();
    const size_t workerCount = m_meshPool
        ? m_meshPool->threadCount()
        : 0;
    if (workerCount > 0) {
        limit = std::min(limit, workerCount);
    } else {
        // Inline jobs complete during dispatch, but remain owned until the
        // main-thread drain observes the single executor slot.
        limit = std::min(limit, static_cast<size_t>(1));
    }
    return limit;
}

void ChunkStreamer::dispatchPendingMeshes(
    uint64_t& schedulerCoordinatesInspected) {
    const size_t dispatchLimit = meshDispatchLimit();
    if (m_inFlightMesh >= dispatchLimit || m_pendingMeshes.empty()) {
        return;
    }

    PROFILE_SCOPE("Streaming/Update/MeshDispatch");

    auto cleanQueue = [&](MeshRequestKind kind) {
        PendingMeshQueue& queue =
            m_pendingMeshQueues[static_cast<size_t>(kind)];
        while (!queue.empty()) {
            const PendingMeshRequest& request = queue.top();
            ++schedulerCoordinatesInspected;
            auto pendingIt = m_pendingMeshes.find(request.coord);
            if (pendingIt == m_pendingMeshes.end() ||
                pendingIt->second.sequence != request.sequence ||
                pendingIt->second.kind != kind) {
                queue.pop();
                continue;
            }
            break;
        }
    };

    const auto higherPriorityKind = [&](const PendingMeshRequest& missing,
                                        const PendingMeshRequest& dirty) {
        return PendingMeshRequestGreater{}(missing, dirty)
            ? MeshRequestKind::Dirty
            : MeshRequestKind::Missing;
    };

    while (m_inFlightMesh < dispatchLimit && !m_pendingMeshes.empty()) {
        cleanQueue(MeshRequestKind::Missing);
        cleanQueue(MeshRequestKind::Dirty);
        PendingMeshQueue& missingQueue =
            m_pendingMeshQueues[static_cast<size_t>(MeshRequestKind::Missing)];
        PendingMeshQueue& dirtyQueue =
            m_pendingMeshQueues[static_cast<size_t>(MeshRequestKind::Dirty)];
        const bool hasMissing = !missingQueue.empty();
        const bool hasDirty = !dirtyQueue.empty();
        if (!hasMissing && !hasDirty) {
            break;
        }

        MeshRequestKind selectedKind = hasMissing
            ? MeshRequestKind::Missing
            : MeshRequestKind::Dirty;
        if (hasMissing && hasDirty) {
            const PendingMeshRequest& missing = missingQueue.top();
            const PendingMeshRequest& dirty = dirtyQueue.top();
            if (missing.prioritized || dirty.prioritized) {
                selectedKind = higherPriorityKind(missing, dirty);
            } else if (dispatchLimit == 1) {
                selectedKind = m_nextSingleSlotMeshKind;
            } else if (dispatchLimit != std::numeric_limits<size_t>::max()) {
                size_t dirtySlots = std::max<size_t>(1, dispatchLimit / 4);
                if (dirtySlots >= dispatchLimit) {
                    dirtySlots = dispatchLimit - 1;
                }
                const size_t missingSlots = dispatchLimit - dirtySlots;
                if (m_inFlightMeshMissing >= missingSlots) {
                    selectedKind = MeshRequestKind::Dirty;
                } else if (m_inFlightMeshDirty >= dirtySlots) {
                    selectedKind = MeshRequestKind::Missing;
                } else {
                    selectedKind = higherPriorityKind(missing, dirty);
                }
            } else {
                selectedKind = higherPriorityKind(missing, dirty);
            }
        }

        PendingMeshQueue& selectedQueue =
            m_pendingMeshQueues[static_cast<size_t>(selectedKind)];
        PendingMeshRequest request = selectedQueue.top();
        selectedQueue.pop();
        ++schedulerCoordinatesInspected;
        auto pendingIt = m_pendingMeshes.find(request.coord);
        if (pendingIt == m_pendingMeshes.end() ||
            pendingIt->second.sequence != request.sequence ||
            pendingIt->second.kind != selectedKind) {
            continue;
        }
        request = pendingIt->second;

        const auto priority = dirtyMeshPriority(request.coord);
        if (!priority) {
            m_priorityMeshRequests.erase(request.coord);
            erasePendingMesh(request.coord);
            continue;
        }

        Chunk* chunk = m_chunkManager->getChunk(request.coord);
        if (!chunk) {
            erasePendingMesh(request.coord);
            if (m_desiredSet.find(request.coord) != m_desiredSet.end()) {
                queueLoadGen(request.coord);
            } else {
                m_priorityMeshRequests.erase(request.coord);
            }
            continue;
        }
        if (m_generator &&
            chunk->worldGenVersion() != m_generator->semanticsVersion()) {
            erasePendingMesh(request.coord);
            m_priorityMeshRequests.erase(request.coord);
            if (m_desiredSet.find(request.coord) != m_desiredSet.end()) {
                queueLoadGen(request.coord);
            }
            continue;
        }

        auto flightIt = m_meshInFlight.find(request.coord);
        if (flightIt != m_meshInFlight.end()) {
            if (flightIt->second.obsolete) {
                if (hasEligibleMeshWork(request.coord)) {
                    setReplacementPending(
                        request.coord, flightIt->second, true);
                    flightIt->second.prioritized =
                        flightIt->second.prioritized || request.prioritized;
                } else {
                    setReplacementPending(
                        request.coord, flightIt->second, false);
                    flightIt->second.prioritized = false;
                }
            } else {
                if (chunk->isDirty() &&
                    flightIt->second.observedRevision !=
                        chunk->meshRevision()) {
                    flightIt->second.observedRevision = chunk->meshRevision();
                    ++m_workMetrics.meshInvalidations;
                    ++m_workMetrics.meshRequestsCoalesced;
                }
                flightIt->second.prioritized =
                    flightIt->second.prioritized || request.prioritized;
            }
            m_priorityMeshRequests.erase(request.coord);
            erasePendingMesh(request.coord);
            continue;
        }

        ChunkState state = ChunkState::Missing;
        auto stateIt = m_states.find(request.coord);
        if (stateIt != m_states.end()) {
            state = stateIt->second;
        }
        const bool hasMesh =
            m_meshStore && m_meshStore->contains(request.coord);
        const bool isMeshed = hasMesh || state == ChunkState::ReadyMesh ||
            m_worldBoundsSuppressedMeshes.find(request.coord) !=
                m_worldBoundsSuppressedMeshes.end();
        if (isMeshed && !chunk->isDirty()) {
            m_priorityMeshRequests.erase(request.coord);
            erasePendingMesh(request.coord);
            continue;
        }

        const MeshRequestKind actualKind = isMeshed
            ? MeshRequestKind::Dirty
            : MeshRequestKind::Missing;
        if (request.kind != actualKind) {
            pendingIt->second.kind = actualKind;
            pendingIt->second.sequence = m_nextPendingMeshSequence++;
            if (m_nextPendingMeshSequence == 0) {
                m_nextPendingMeshSequence = 1;
            }
            m_pendingMeshQueues[static_cast<size_t>(actualKind)].push(
                pendingIt->second);
            compactPendingMeshQueuesIfNeeded();
            continue;
        }
        if (!hasAllNeighborsLoaded(request.coord)) {
            waitForMeshDependencies(request.coord);
            continue;
        }

        erasePendingMesh(request.coord);
        enqueueMesh(
            request.coord,
            *chunk,
            request.kind,
            request.prioritized);
    }
}

void ChunkStreamer::enqueueGeneration(ChunkCoord coord) {
    if (m_config.genQueueLimit > 0 &&
        m_inFlightGen >= m_config.genQueueLimit) {
        return;
    }

    m_configRetiredWork.erase(coord);
    ensureVisibilityTrace(
        coord,
        ChunkVisibilityLifecycleKind::CameraDemand);
    markVisibilityStage(coord, ChunkVisibilityStage::DataRequest);
    markVisibilityStage(
        coord, ChunkVisibilityStage::GenerationSchedulerPending);
    const auto visibilityLink = currentVisibilityTrace(
        coord, ChunkVisibilityLifecycleKind::CameraDemand);

    m_states[coord] = ChunkState::QueuedGen;
    ++m_inFlightGen;

    auto flight = std::make_shared<GenerationFlight>();
    m_generationFlights[coord] = flight;
    auto generator = m_generator;
    const uint32_t worldGenVersion = generator
        ? generator->semanticsVersion()
        : 0;
    uint64_t workEpoch = m_workEpoch.load(std::memory_order_relaxed);
    auto generationStartCallback = m_generationStartCallback;
    auto generationStartObserver = m_generationStartObserver;
    auto generationResultReadyToPublishCallback =
        m_generationResultReadyToPublishCallback;
    auto generationResultPublishedObserver =
        m_generationResultPublishedObserver;
    auto job = [this,
                generator,
                coord,
                flight,
                visibilityLink,
                worldGenVersion,
                workEpoch,
                generationStartCallback = std::move(generationStartCallback),
                generationStartObserver = std::move(generationStartObserver),
                generationResultReadyToPublishCallback = std::move(
                    generationResultReadyToPublishCallback),
                generationResultPublishedObserver = std::move(
                    generationResultPublishedObserver)]() {
        flight->phase.store(
            GenerationExecutorPhase::WorkerRunning,
            std::memory_order_release);
        GenResult result;
        result.coord = coord;
        result.workEpoch = workEpoch;
        result.worldGenVersion = worldGenVersion;
        result.flight = flight;
        if (visibilityLink) {
            result.visibilityTrace = visibilityLink->key;
            result.visibilityTracer = visibilityLink->tracer;
            visibilityLink->tracer->mark(
                visibilityLink->key,
                ChunkVisibilityStage::GenerationWorkerStart);
        }
        auto publish = [&]() {
            if (result.visibilityTracer && result.visibilityTrace) {
                result.visibilityTracer->mark(
                    *result.visibilityTrace,
                    ChunkVisibilityStage::GenerationWorkerFinish);
            }
            if (generationResultReadyToPublishCallback) {
                generationResultReadyToPublishCallback();
            }
            const auto visibilityTracer = result.visibilityTracer;
            const auto visibilityTrace = result.visibilityTrace;
            m_genComplete.push(
                std::move(result),
                [flight,
                 visibilityTracer,
                 visibilityTrace,
                 generationResultPublishedObserver,
                 coord]() noexcept {
                    flight->phase.store(
                        GenerationExecutorPhase::ResultPublished,
                        std::memory_order_release);
                    if (visibilityTracer && visibilityTrace) {
                        try {
                            visibilityTracer->mark(
                                *visibilityTrace,
                                ChunkVisibilityStage::GenerationReady);
                        } catch (...) {
                        }
                    }
                    if (generationResultPublishedObserver) {
                        try {
                            generationResultPublishedObserver(coord);
                        } catch (...) {
                        }
                    }
                });
        };
        if (flight->cancelled.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            publish();
            return;
        }

        try {
            if (generationStartObserver) {
                generationStartObserver(coord);
            }
            if (generationStartCallback) {
                generationStartCallback();
            }
            if (workEpoch != m_workEpoch.load(std::memory_order_relaxed) ||
                flight->cancelled.load(std::memory_order_relaxed)) {
                result.cancelled = true;
                publish();
                return;
            }
            ChunkBuffer buffer;
            auto start = std::chrono::steady_clock::now();
            generator->generate(coord, buffer, &flight->cancelled);
            auto end = std::chrono::steady_clock::now();

            result.blocks = buffer.blocks;
            result.seconds = std::chrono::duration<double>(end - start).count();
        } catch (const std::exception& e) {
            result.failed = true;
            result.error = e.what();
        } catch (...) {
            result.failed = true;
            result.error = "unknown error";
        }
        result.cancelled = flight->cancelled.load(std::memory_order_relaxed);
        publish();
    };

    if (visibilityLink) {
        visibilityLink->tracer->mark(
            visibilityLink->key,
            ChunkVisibilityStage::GenerationExecutorSubmit);
    }
    flight->phase.store(
        GenerationExecutorPhase::ExecutorQueued,
        std::memory_order_release);
    if (m_genPool && m_genPool->threadCount() > 0) {
        auto executorJob = m_genPool->enqueueCancellable(std::move(job));
        if (!executorJob) {
            flight->cancelled.store(true, std::memory_order_relaxed);
            auto flightIt = m_generationFlights.find(coord);
            bool erasedFlight = false;
            if (flightIt != m_generationFlights.end() &&
                flightIt->second == flight) {
                m_generationFlights.erase(flightIt);
                erasedFlight = true;
            }
            auto stateIt = m_states.find(coord);
            if (stateIt != m_states.end() &&
                stateIt->second == ChunkState::QueuedGen) {
                m_states.erase(stateIt);
            }
            assert(erasedFlight);
            assert(m_inFlightGen > 0);
            if (erasedFlight && m_inFlightGen > 0) {
                --m_inFlightGen;
            }
            completePendingVisibilityTrace(
                coord, ChunkVisibilityOutcome::Failed);
            return;
        }
        flight->executorJob = std::move(executorJob);
        ++m_workMetrics.generationJobsStarted;
    }
    if (!m_genPool || m_genPool->threadCount() == 0) {
        ++m_workMetrics.generationJobsStarted;
        job();
    }
}

void ChunkStreamer::enqueueMesh(ChunkCoord coord,
                                Chunk& chunk,
                                MeshRequestKind kind,
                                bool prioritized) {
    if (!m_registry) {
        return;
    }
    if (m_inFlightMesh >= meshDispatchLimit()) {
        return;
    }
    if (m_meshInFlight.find(coord) != m_meshInFlight.end()) {
        return;
    }

    m_configRetiredWork.erase(coord);
    prioritized = prioritized ||
        m_priorityMeshRequests.find(coord) != m_priorityMeshRequests.end();

    chunk.clearDirty();

    MeshTask task;
    task.coord = coord;
    task.requestId = m_nextMeshRequestId++;
    task.workEpoch = m_workEpoch.load(std::memory_order_relaxed);
    if (m_nextMeshRequestId == 0) {
        m_nextMeshRequestId = 1;
    }
    task.chunkInstanceId = chunk.m_instanceId;
    task.revision = chunk.meshRevision();
    chunk.copyBlocks(task.blocks);
    const auto& meshWorld = m_generator->definition().bounds;
    const int64_t chunkMinWorldY =
        static_cast<int64_t>(coord.y) * Chunk::SIZE;
    auto sampleInsideWorld = [&](int localY) {
        const int64_t worldY = chunkMinWorldY + localY;
        return worldY >= meshWorld.minY && worldY <= meshWorld.maxY;
    };
    BlockState air;
    for (int y = 0; y < Chunk::SIZE; ++y) {
        if (sampleInsideWorld(y)) {
            continue;
        }
        for (int z = 0; z < Chunk::SIZE; ++z) {
            for (int x = 0; x < Chunk::SIZE; ++x) {
                task.blocks[
                    x + y * Chunk::SIZE +
                    z * Chunk::SIZE * Chunk::SIZE] = air;
            }
        }
    }

    std::array<const Chunk*, 27> neighborChunks{};
    auto neighborIndex = [](int dx, int dy, int dz) {
        return (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9;
    };
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord neighborCoord = coord.offset(dx, dy, dz);
                neighborChunks[neighborIndex(dx, dy, dz)] =
                    chunkIntersectsWorldBounds(neighborCoord)
                    ? m_chunkManager->getChunk(neighborCoord)
                    : nullptr;
            }
        }
    }
    neighborChunks[neighborIndex(0, 0, 0)] = &chunk;

    for (int pz = 0; pz < kPaddedSize; ++pz) {
        int lz = pz - 1;
        for (int py = 0; py < kPaddedSize; ++py) {
            int ly = py - 1;
            for (int px = 0; px < kPaddedSize; ++px) {
                int lx = px - 1;

                int ox = 0;
                int oy = 0;
                int oz = 0;
                int sx = lx;
                int sy = ly;
                int sz = lz;

                if (sx < 0) {
                    ox = -1;
                    sx += Chunk::SIZE;
                } else if (sx >= Chunk::SIZE) {
                    ox = 1;
                    sx -= Chunk::SIZE;
                }
                if (sy < 0) {
                    oy = -1;
                    sy += Chunk::SIZE;
                } else if (sy >= Chunk::SIZE) {
                    oy = 1;
                    sy -= Chunk::SIZE;
                }
                if (sz < 0) {
                    oz = -1;
                    sz += Chunk::SIZE;
                } else if (sz >= Chunk::SIZE) {
                    oz = 1;
                    sz -= Chunk::SIZE;
                }

                const Chunk* source = neighborChunks[neighborIndex(ox, oy, oz)];
                size_t index = static_cast<size_t>(px)
                    + static_cast<size_t>(py) * kPaddedSize
                    + static_cast<size_t>(pz) * kPaddedSize * kPaddedSize;
                if (source && sampleInsideWorld(ly)) {
                    task.paddedBlocks[index] = source->getBlock(sx, sy, sz);
                } else {
                    task.paddedBlocks[index] = air;
                }
            }
        }
    }

    const ChunkVisibilityLifecycleKind visibilityKind =
        kind == MeshRequestKind::Dirty
        ? ChunkVisibilityLifecycleKind::Remesh
        : ChunkVisibilityLifecycleKind::CameraDemand;
    const auto visibilityLink = bindVisibilityTrace(
        coord,
        ChunkVisibilityMeshTaskIdentity{
            task.requestId,
            task.workEpoch,
            task.chunkInstanceId,
            task.revision
        },
        visibilityKind);
    if (visibilityLink) {
        task.visibilityTrace = visibilityLink->key;
        task.visibilityTracer = visibilityLink->tracer;
        task.visibilityKind = visibilityLink->kind;
    }

    m_states[coord] = ChunkState::QueuedMesh;
    ++m_inFlightMesh;
    m_meshInFlight[coord] = MeshInFlight{
        .kind = kind,
        .requestId = task.requestId,
        .workEpoch = task.workEpoch,
        .observedRevision = task.revision,
        .prioritized = prioritized,
        .visibilityTrace = task.visibilityTrace,
        .visibilityTracer = task.visibilityTracer,
        .visibilityKind = task.visibilityKind
    };
    m_priorityMeshRequests.erase(coord);
    ++m_workMetrics.meshJobsStarted;
    if (kind == MeshRequestKind::Missing) {
        ++m_inFlightMeshMissing;
    } else {
        ++m_inFlightMeshDirty;
        auto retryIt = m_countedMeshRetryRevisions.find(coord);
        bool invalidationAlreadyCounted = retryIt != m_countedMeshRetryRevisions.end() &&
            retryIt->second == task.revision;
        if (retryIt != m_countedMeshRetryRevisions.end()) {
            m_countedMeshRetryRevisions.erase(retryIt);
        }
        if (!invalidationAlreadyCounted) {
            ++m_workMetrics.meshInvalidations;
        }
    }
    if (meshDispatchLimit() == 1) {
        m_nextSingleSlotMeshKind = kind == MeshRequestKind::Missing
            ? MeshRequestKind::Dirty
            : MeshRequestKind::Missing;
    }

    BlockRegistry* registry = m_registry;
    TextureAtlas* atlas = m_atlas;
    const auto visibilityTrace = task.visibilityTrace;
    const auto visibilityTracer = task.visibilityTracer;
    auto meshBuildStartCallback = m_meshBuildStartCallback;
    auto job = [this,
                task = std::move(task),
                registry,
                atlas,
                meshBuildStartCallback = std::move(meshBuildStartCallback)]() mutable {
        MeshResult result;
        result.coord = task.coord;
        result.requestId = task.requestId;
        result.workEpoch = task.workEpoch;
        result.chunkInstanceId = task.chunkInstanceId;
        result.revision = task.revision;
        result.visibilityTrace = task.visibilityTrace;
        result.visibilityTracer = task.visibilityTracer;
        if (result.visibilityTracer && result.visibilityTrace) {
            result.visibilityTracer->mark(
                *result.visibilityTrace,
                ChunkVisibilityStage::WorkerStart);
        }
        try {
            if (meshBuildStartCallback) {
                meshBuildStartCallback();
            }
            if (task.workEpoch ==
                m_workEpoch.load(std::memory_order_relaxed)) {
                Chunk chunk(task.coord);
                chunk.copyFrom(task.blocks);

                std::array<const Chunk*, DirectionCount> neighborPtrs{};

                MeshBuilder builder;
                MeshBuilder::BuildContext ctx{
                    .chunk = chunk,
                    .registry = *registry,
                    .atlas = atlas,
                    .neighbors = neighborPtrs,
                    .paddedBlocks = &task.paddedBlocks
                };

                auto start = std::chrono::steady_clock::now();
                result.mesh = builder.build(ctx);
                auto end = std::chrono::steady_clock::now();

                result.seconds =
                    std::chrono::duration<double>(end - start).count();
                result.empty = result.mesh.isEmpty();
            }
        } catch (const std::exception& e) {
            result.failed = true;
            result.error = e.what();
        } catch (...) {
            result.failed = true;
            result.error = "unknown error";
        }
        if (result.visibilityTracer && result.visibilityTrace) {
            result.visibilityTracer->mark(
                *result.visibilityTrace,
                ChunkVisibilityStage::WorkerFinish);
        }
        m_meshComplete.push(std::move(result));
    };

    if (visibilityTracer && visibilityTrace) {
        visibilityTracer->mark(
            *visibilityTrace,
            ChunkVisibilityStage::PoolSubmit);
    }
    if (m_meshPool && m_meshPool->threadCount() > 0) {
        m_meshPool->enqueue(std::move(job));
    } else {
        job();
    }
}

void ChunkStreamer::ensureThreadPool() {
    size_t desired = 0;
    if (m_config.workerThreads > 0) {
        desired = static_cast<size_t>(m_config.workerThreads);
    }
    if (desired == 0) {
        m_genPool.reset();
        m_meshPool.reset();
        return;
    }
    size_t meshThreads = desired / 2;
    size_t genThreads = desired - meshThreads;
    if (meshThreads == 0) {
        meshThreads = 0;
    }
    if (!m_genPool || m_genPool->threadCount() != genThreads) {
        m_genPool = std::make_unique<detail::ThreadPool>(genThreads);
    }
    if (!m_meshPool || m_meshPool->threadCount() != meshThreads) {
        m_meshPool = std::make_unique<detail::ThreadPool>(meshThreads);
    }
}

uint8_t ChunkStreamer::countMissingDesiredCardinalNeighbors(
    ChunkCoord coord,
    uint8_t limit) const {
    limit = std::min(limit, static_cast<uint8_t>(DirectionCount));
    if (limit == 0) {
        return 0;
    }
    if (!m_chunkManager) {
        return limit;
    }
    uint8_t missing = 0;
    for (int i = 0; i < DirectionCount; ++i) {
        Direction dir = static_cast<Direction>(i);
        int dx = 0;
        int dy = 0;
        int dz = 0;
        directionOffset(dir, dx, dy, dz);
        ChunkCoord neighbor = coord.offset(dx, dy, dz);
        if (m_chunkManager->getChunk(neighbor)) {
            continue;
        }
        if (m_desiredSet.find(neighbor) == m_desiredSet.end()) {
            continue;
        }
        if (++missing == limit) {
            break;
        }
    }
    return missing;
}

bool ChunkStreamer::hasAllNeighborsLoaded(ChunkCoord coord) const {
    return countMissingDesiredCardinalNeighbors(coord, 1) == 0;
}

} // namespace Rigel::Voxel
