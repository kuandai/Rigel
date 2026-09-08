#pragma once

#include "Rigel/Simulation/SimulationHost.h"
#include "Rigel/input/GameplayInput.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace Rigel::Voxel {
class World;
}
namespace Rigel::Asset {
class AssetManager;
}

namespace Rigel::detail {

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
        std::string placeBlockKey);

    bool submit(
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
    Simulation::Revision revision() const { return m_replica.revision(); }
    Simulation::Tick tick() const { return m_replica.tick(); }

private:
    void drainPublications();
    void apply(const Simulation::PublicationMessage& publication);
    void applyCell(const Simulation::PublishedCell& cell);
    void applyEntities(const std::vector<Simulation::PublishedEntity>& entities);

    Simulation::SimulationHost* m_host = nullptr;
    Voxel::World* m_replicaWorld = nullptr;
    Asset::AssetManager* m_assets = nullptr;
    Simulation::LoopbackReplica m_replica;
    Simulation::LocalObserverCapability m_observerCapability;
    Entity::EntityId m_observer;
    Simulation::SessionId m_session = 0;
    Simulation::CommandId m_nextCommand = 1;
    std::string m_placeBlockKey;
    std::vector<Voxel::ChunkCoord> m_changedChunks;
    std::vector<Simulation::CommandOutcome> m_outcomes;
};

} // namespace Rigel::detail
