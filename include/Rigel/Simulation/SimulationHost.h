#pragma once

#include "ContentManifest.h"

#include <Rigel/Entity/EntityId.h>
#include <Rigel/Entity/Entity.h>
#include <Rigel/Voxel/Block.h>
#include <Rigel/Voxel/BlockTargeting.h>
#include <Rigel/Voxel/ChunkCoord.h>
#include <Rigel/Voxel/WorldId.h>

#include <chrono>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Rigel::Voxel {
class World;
class WorldGenerator;
class WorldResources;
}

namespace Rigel::Entity {
class Entity;
}

namespace Rigel::Simulation {

class SimulationCheckpointManager;

struct CellAddress {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const CellAddress&) const = default;
    auto operator<=>(const CellAddress&) const = default;
};

struct CellBounds {
    CellAddress min;
    CellAddress max;

    bool contains(CellAddress address) const;
    std::optional<size_t> volume(size_t limit) const;
    bool operator==(const CellBounds&) const = default;
};

enum class ExactReadStatus {
    Known,
    Unavailable,
    OutsideDomain,
    InvalidState,
};

struct ExactBlockRead {
    ExactReadStatus status = ExactReadStatus::Unavailable;
    SemanticBlockState state;
};

using SessionId = uint64_t;
using CommandId = uint64_t;
using Tick = uint64_t;
using Revision = uint64_t;

enum class EditAction {
    Remove,
    Place,
    Atomic,
};

class SimulationHost;

/** Explicit host-bound authority required for multi-cell edits. */
class AuthorityEditCapability final {
public:
    AuthorityEditCapability(const AuthorityEditCapability&) = default;

private:
    explicit AuthorityEditCapability(std::weak_ptr<const uint8_t> owner)
        : m_owner(std::move(owner)) {}

    std::weak_ptr<const uint8_t> m_owner;
    friend class SimulationHost;
};

/** Host-bound permission for the existing trusted local free-fly observer. */
class LocalObserverCapability final {
public:
    LocalObserverCapability(const LocalObserverCapability&) = default;

private:
    explicit LocalObserverCapability(std::weak_ptr<const uint8_t> owner)
        : m_owner(std::move(owner)) {}

    std::weak_ptr<const uint8_t> m_owner;
    friend class SimulationHost;
};

struct InteractionIntent {
    glm::vec3 origin{};
    glm::vec3 direction{};
    float maxDistance = 0.0f;
    CellAddress expectedTarget;
    Voxel::Direction expectedFace = Voxel::Direction::PosX;
    SemanticBlockState expectedTargetState;

    bool operator==(const InteractionIntent&) const = default;
};

struct CellMutation {
    CellAddress address;
    SemanticBlockState expected;
    SemanticBlockState replacement;

    bool operator==(const CellMutation&) const = default;
};

struct EditCommand {
    SessionId session = 0;
    CommandId command = 0;
    Entity::EntityId actor;
    Voxel::WorldId world = Voxel::kDefaultWorldId;
    std::string zone;
    ContentManifestId content;
    EditAction action = EditAction::Remove;
    std::optional<InteractionIntent> interaction;
    std::vector<CellMutation> mutations;

    bool operator==(const EditCommand&) const = default;
};

enum class CommandOutcomeStatus {
    Applied,
    NoChange,
    StaleState,
    TargetMismatch,
    Unavailable,
    OutsideDomain,
    InvalidRequest,
    ActorUnavailable,
    PlacementCollision,
};

struct CommandOutcome {
    SessionId session = 0;
    CommandId command = 0;
    uint64_t admission = 0;
    Tick tick = 0;
    Revision revision = 0;
    CommandOutcomeStatus status = CommandOutcomeStatus::InvalidRequest;

    bool operator==(const CommandOutcome&) const = default;
};

enum class SubmitStatus {
    Accepted,
    DuplicatePending,
    DuplicateComplete,
    PayloadConflict,
    OldSession,
    ContentMismatch,
    WrongDomain,
    InvalidRequest,
    ReceiptCapacity,
    CommandCapacity,
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::InvalidRequest;
    std::optional<CommandOutcome> outcome;
};

struct PublishedCell {
    CellAddress address;
    SemanticBlockState state;

    bool operator==(const PublishedCell&) const = default;
};

struct PublishedEntity {
    Entity::EntitySimulationState state;

    bool operator==(const PublishedEntity&) const = default;
};

struct WorldBaseline {
    ContentManifestId content;
    Voxel::WorldId world = Voxel::kDefaultWorldId;
    std::string zone;
    CellBounds bounds;
    Tick tick = 0;
    Revision revision = 0;
    bool complete = true;
    std::vector<PublishedCell> cells;
    std::vector<PublishedEntity> entities;
};

struct WorldChangeBatch {
    ContentManifestId content;
    Voxel::WorldId world = Voxel::kDefaultWorldId;
    std::string zone;
    Revision baseRevision = 0;
    Revision revision = 0;
    Tick tick = 0;
    bool complete = true;
    std::vector<PublishedCell> changes;
    std::vector<PublishedEntity> entities;
    std::vector<CommandOutcome> outcomes;
};

using PublicationMessage = std::variant<WorldBaseline, WorldChangeBatch>;

enum class ReplicaAcceptStatus {
    Queued,
    QueueFull,
    RejectedOversized,
};

enum class ReplicaPumpStatus {
    Idle,
    Applied,
    Duplicate,
    NeedsResnapshot,
};

class LoopbackReplica final {
public:
    LoopbackReplica(const LoopbackReplica&) = delete;
    LoopbackReplica& operator=(const LoopbackReplica&) = delete;
    LoopbackReplica(LoopbackReplica&&) noexcept = default;
    LoopbackReplica& operator=(LoopbackReplica&&) noexcept = default;

    ReplicaAcceptStatus accept(std::shared_ptr<const PublicationMessage> message);
    ReplicaPumpStatus pumpOne();
    ExactBlockRead read(CellAddress address) const;
    std::optional<CommandOutcome> takeOutcome();
    /** Immutable message accepted by the most recent successful pump. The
     * pointer remains valid until the next pump or resnapshot operation. */
    const PublicationMessage* appliedPublication() const;

    bool needsResnapshot() const;
    Revision revision() const;
    Tick tick() const;
    size_t queuedMessages() const;

private:
    struct State;
    explicit LoopbackReplica(std::shared_ptr<State> state)
        : m_state(std::move(state)) {}

    std::shared_ptr<State> m_state;
    friend class SimulationHost;
};

enum class SessionStartStatus {
    Started,
    OldSession,
    Busy,
    ContentMismatch,
    ActorUnavailable,
};

enum class ObserverPoseStatus {
    Applied,
    InvalidCapability,
    ActorUnavailable,
    UnsupportedActor,
    InvalidPosition,
};

enum class ReplicaConnectStatus {
    Connected,
    NotNeeded,
    InvalidInterest,
    Capacity,
};

struct ReplicaConnection {
    ReplicaConnectStatus status = ReplicaConnectStatus::InvalidInterest;
    std::optional<LoopbackReplica> replica;
};

struct ActiveSessionState {
    SessionId session = 0;
    Entity::EntityId actor;
    CommandId nextCommand = 1;
    std::vector<CommandId> pendingCommands;
};

struct TickRate {
    uint32_t numerator = 60;
    uint32_t denominator = 1;
};

struct SimulationHostConfig {
    Voxel::WorldId world = Voxel::kDefaultWorldId;
    std::string zone = "base:default";
    CellBounds domain{{0, 0, 0}, {31, 31, 31}};
    std::vector<Voxel::ChunkCoord> preloadedChunks;
    TickRate tickRate;
    size_t maxPreloadedChunks = 8;
    size_t maxSnapshotCells = 262'144;
    size_t maxChangesPerCommand = 8;
    size_t maxPendingCommands = 64;
    size_t maxSessionReceipts = 256;
    size_t maxReplicas = 8;
    size_t maxReplicaQueue = 32;
    size_t maxEntities = 256;
    size_t maxEntityTags = 16;
    size_t maxEntityTagBytes = 1024;
    size_t maxCommandBytes = 64 * 1024;
    size_t maxContentBytes = ContentDictionary::kDefaultMaxRetainedBytes;
    size_t maxReplicaBytes = 64 * 1024 * 1024;
    float maxInteractionDistance = 8.0f;
    size_t maxCatchUpTicks = 8;
    size_t maxCheckpointBytes = 64 * 1024 * 1024;
    size_t maxReplayEvents = 4096;
    size_t maxReplayBytes = 64 * 1024 * 1024;
};

struct AdvanceResult {
    size_t ticksRun = 0;
    bool timeDebtRemaining = false;
};

struct SimulationRecording {
    ContentManifestId content;
    Tick finalTick = 0;
    uint64_t finalHash = 0;
    std::vector<uint8_t> bytes;
};

enum class ResimulationStatus {
    Complete,
    EnvelopeMismatch,
    MalformedRecording,
    Diverged,
};

struct ResimulationResult {
    ResimulationStatus status = ResimulationStatus::MalformedRecording;
    Tick tick = 0;
    uint64_t stateHash = 0;
    std::unique_ptr<SimulationHost> host;
};

class SimulationHost final {
public:
    SimulationHost(
        Voxel::WorldResources& resources,
        std::shared_ptr<const Voxel::WorldGenerator> generator,
        SimulationHostConfig config = {});
    ~SimulationHost();

    SimulationHost(const SimulationHost&) = delete;
    SimulationHost& operator=(const SimulationHost&) = delete;

    const ContentDictionary& content() const { return *m_content; }
    const Voxel::World& world() const;
    Tick tick() const { return m_tick; }
    Revision revision() const { return m_revision; }
    ExactBlockRead read(CellAddress address) const;
    Entity::EntityId spawnEntity(std::unique_ptr<Entity::Entity> entity);
    bool despawnEntity(Entity::EntityId entity);

    SessionStartStatus startSession(
        SessionId session,
        Entity::EntityId actor,
        const ContentManifestId& content);
    /** Bounded immutable cursor used to reattach a local client after
     * checkpoint recovery without replacing admitted commands. */
    std::optional<ActiveSessionState> activeSession() const;
    AuthorityEditCapability authorityEditCapability() const {
        return AuthorityEditCapability(m_authorityEditKey);
    }
    LocalObserverCapability localObserverCapability() const {
        return LocalObserverCapability(m_authorityEditKey);
    }
    /** Admit a trusted local developer-camera pose between ticks. The
     * admission is part of recordings and is visible to later commands and
     * the next checkpoint cut. It is not a general movement proposal. */
    ObserverPoseStatus admitLocalObserverPose(
        Entity::EntityId actor,
        const glm::vec3& position,
        const LocalObserverCapability& capability);
    SessionId nextSessionId() const;
    SubmitResult submit(EditCommand command);
    SubmitResult submit(
        EditCommand command,
        const AuthorityEditCapability& capability);
    AdvanceResult advance(std::chrono::nanoseconds elapsed);

    ReplicaConnection connectReplica(CellBounds interest);
    ReplicaConnectStatus resnapshot(LoopbackReplica& replica);

    /** Canonical semantic hash for a completed or inter-tick authority cut. */
    uint64_t stateHash() const;
    /** State baseline plus all accepted inter-tick admissions since construction. */
    std::optional<SimulationRecording> recording() const;
    static ResimulationResult resimulate(
        Voxel::WorldResources& resources,
        std::shared_ptr<const Voxel::WorldGenerator> generator,
        const SimulationRecording& recording,
        const std::vector<std::chrono::nanoseconds>& framePacing);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::shared_ptr<const uint8_t> m_authorityEditKey =
        std::make_shared<const uint8_t>(0);
    std::shared_ptr<const ContentDictionary> m_content;
    SimulationHostConfig m_config;
    Tick m_tick = 0;
    Revision m_revision = 0;
    uint64_t m_timeDebt = 0;
    uint32_t m_nextEntityId = 1;

    SubmitResult submit(EditCommand command, bool privileged);
    void runTick();

    std::vector<uint8_t> checkpointBytes(
        uint64_t generation, uint64_t parentHash,
        bool includeTimeDebt = true) const;
    bool prepareRecordingBaseline();
    bool recordingWithinLimit();
    void discardRecording();
    static std::unique_ptr<SimulationHost> restoreCheckpointBytes(
        Voxel::WorldResources& resources,
        std::shared_ptr<const Voxel::WorldGenerator> generator,
        const std::vector<uint8_t>& bytes,
        uint64_t expectedGeneration,
        uint64_t expectedParentHash);

    friend class SimulationCheckpointManager;
};

} // namespace Rigel::Simulation
