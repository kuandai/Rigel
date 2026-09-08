#pragma once

#include "SimulationHost.h"

#include <Rigel/Persistence/Storage.h>

#include <memory>
#include <optional>
#include <string>

namespace Rigel::Simulation {

enum class CheckpointRequestStatus {
    Started,
    Coalesced,
    Uncertain,
    UnsupportedState,
};

enum class CheckpointWriteStatus {
    Durable,
    NotPublished,
    DurabilityUnknown,
};

struct CheckpointOutcome {
    CheckpointWriteStatus status = CheckpointWriteStatus::NotPublished;
    uint64_t generation = 0;
    Tick tick = 0;
    Revision revision = 0;
    uint64_t stateHash = 0;
    std::string detail;
};

enum class CheckpointRecoveryStatus {
    Recovered,
    Empty,
    Incompatible,
    Corrupt,
};

struct CheckpointRecovery {
    CheckpointRecoveryStatus status = CheckpointRecoveryStatus::Empty;
    uint64_t generation = 0;
    std::unique_ptr<SimulationHost> host;
    std::string detail;
};

/**
 * Exclusive live publisher for one checkpoint root. The root lock remains held
 * until the writer has reached a terminal outcome and the manager is destroyed.
 */
class SimulationCheckpointManager final {
public:
    SimulationCheckpointManager(
        std::shared_ptr<Persistence::StorageBackend> storage,
        std::string root);
    ~SimulationCheckpointManager();

    SimulationCheckpointManager(const SimulationCheckpointManager&) = delete;
    SimulationCheckpointManager& operator=(const SimulationCheckpointManager&) = delete;

    CheckpointRequestStatus request(const SimulationHost& host);
    std::optional<CheckpointOutcome> poll();
    bool writeInFlight() const;
    bool durabilityUncertain() const;

    CheckpointRecovery recover(
        Voxel::WorldResources& resources,
        std::shared_ptr<const Voxel::WorldGenerator> generator);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Rigel::Simulation
