#pragma once

#include "Rigel/Simulation/SimulationHost.h"
#include "Rigel/input/GameplayInput.h"

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rigel::Voxel {
class World;
}
namespace Rigel::Asset {
class AssetManager;
}

namespace Rigel::detail {

enum class GraphicalEditSubmitStatus {
    Accepted,
    AcceptedAfterSessionReplacement,
    ObserverRejected,
    ReplicaStateUnavailable,
    SessionReplacementDeferred,
    SessionReplacementFailed,
    HostRejected,
};

struct GraphicalEditSubmitResult {
    GraphicalEditSubmitStatus status =
        GraphicalEditSubmitStatus::HostRejected;
    Simulation::SubmitStatus hostStatus =
        Simulation::SubmitStatus::InvalidRequest;

    bool accepted() const {
        return status == GraphicalEditSubmitStatus::Accepted ||
            status ==
                GraphicalEditSubmitStatus::AcceptedAfterSessionReplacement;
    }
};

struct GraphicalEditSubmissionStats {
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t sessionReplacements = 0;
};

/** Application-thread loopback client for the bounded normal world. It owns
 * no authority storage: publications are copied into an independent World. */
class GraphicalAuthorityClient final {
public:
    GraphicalAuthorityClient(
        Simulation::SimulationHost& host,
        Voxel::World& replicaWorld,
        Asset::AssetManager& assets,
        Simulation::CellBounds interest,
        Entity::EntityId observer,
        Simulation::SessionId session,
        std::string placeBlockKey,
        Simulation::CommandId nextCommand = 1,
        std::vector<Simulation::CommandId> pendingCommands = {});

    GraphicalEditSubmitResult submit(
        Input::GameplayBlockEditAction action,
        const Voxel::BlockTarget& target,
        const Input::CameraState& camera);
    Simulation::AdvanceResult advance(std::chrono::nanoseconds elapsed);

    const std::vector<Voxel::ChunkCoord>& changedChunks() const {
        return m_changedChunks;
    }
    const std::vector<Simulation::CommandOutcome>& outcomes() const {
        return m_outcomes;
    }
    Simulation::Revision revision() const { return m_visibleRevision; }
    Simulation::Tick tick() const { return m_visibleTick; }
    bool projectionRetryPending() const { return m_projectionBlocked; }
    Simulation::SessionId session() const { return m_session; }
    size_t pendingSubmissionCount() const { return m_pendingSubmissions.size(); }
    const GraphicalEditSubmissionStats& submissionStats() const {
        return m_submissionStats;
    }

private:
    bool drainPublications() noexcept;
    void apply(const Simulation::PublicationMessage& publication);
    void deferElapsed(std::chrono::nanoseconds elapsed);

    Simulation::SimulationHost* m_host = nullptr;
    Voxel::World* m_replicaWorld = nullptr;
    Asset::AssetManager* m_assets = nullptr;
    Simulation::LoopbackReplica m_replica;
    Simulation::LocalObserverCapability m_observerCapability;
    Entity::EntityId m_observer;
    Simulation::SessionId m_session = 0;
    Simulation::CommandId m_nextCommand = 1;
    std::set<std::pair<Simulation::SessionId, Simulation::CommandId>>
        m_pendingSubmissions;
    std::string m_placeBlockKey;
    std::vector<Voxel::ChunkCoord> m_changedChunks;
    std::vector<Simulation::CommandOutcome> m_outcomes;
    std::shared_ptr<const Simulation::PublicationMessage> m_pendingPublication;
    GraphicalEditSubmissionStats m_submissionStats;
    Simulation::Revision m_visibleRevision = 0;
    Simulation::Tick m_visibleTick = 0;
    std::chrono::nanoseconds m_deferredElapsed{0};
    bool m_hasVisiblePublication = false;
    bool m_projectionPending = false;
    bool m_projectionBlocked = false;
};

} // namespace Rigel::detail
