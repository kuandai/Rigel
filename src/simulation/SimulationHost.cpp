#include "Rigel/Simulation/SimulationHost.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/RayAabb.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Rigel::Simulation {
namespace {

CellAddress addressOf(const glm::ivec3& value) {
    return {value.x, value.y, value.z};
}

int& component(CellAddress& value, size_t axis) {
    if (axis == 0) return value.x;
    if (axis == 1) return value.y;
    return value.z;
}

bool validBounds(const CellBounds& bounds) {
    return bounds.min.x <= bounds.max.x &&
        bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}

bool boundsWithin(const CellBounds& inner, const CellBounds& outer) {
    return outer.contains(inner.min) && outer.contains(inner.max);
}

bool overlap(const Entity::Aabb& actor, const Voxel::BlockCollisionBox& block) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (actor.min[axis] >= block.max[axis] ||
            actor.max[axis] <= block.min[axis]) return false;
    }
    return true;
}

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool addBytes(size_t& total, size_t value) {
    if (value > std::numeric_limits<size_t>::max() - total) return false;
    total += value;
    return true;
}

bool addElements(size_t& total, size_t count, size_t elementSize) {
    if (count > std::numeric_limits<size_t>::max() / elementSize) return false;
    return addBytes(total, count * elementSize);
}

std::optional<size_t> commandRetainedBytes(const EditCommand& command) {
    size_t total = sizeof(EditCommand);
    if (!addBytes(total, command.zone.capacity()) ||
        !addElements(total, command.mutations.capacity(), sizeof(CellMutation))) {
        return std::nullopt;
    }
    for (const auto& mutation : command.mutations) {
        if (!addBytes(total, mutation.expected.blockKey.capacity()) ||
            !addBytes(total, mutation.replacement.blockKey.capacity())) {
            return std::nullopt;
        }
    }
    if (command.interaction &&
        !addBytes(total, command.interaction->expectedTargetState.blockKey.capacity())) {
        return std::nullopt;
    }
    return total;
}

Voxel::BlockCollisionBox translated(
    const Voxel::BlockCollisionBox& box,
    CellAddress address
) {
    return {
        .min = {box.min[0] + address.x, box.min[1] + address.y,
                box.min[2] + address.z},
        .max = {box.max[0] + address.x, box.max[1] + address.y,
                box.max[2] + address.z},
    };
}

} // namespace

bool CellBounds::contains(CellAddress address) const {
    return address.x >= min.x && address.x <= max.x &&
        address.y >= min.y && address.y <= max.y &&
        address.z >= min.z && address.z <= max.z;
}

std::optional<size_t> CellBounds::volume(size_t limit) const {
    if (!validBounds(*this)) return std::nullopt;
    size_t result = 1;
    for (const auto [low, high] : {
             std::pair{min.x, max.x}, std::pair{min.y, max.y},
             std::pair{min.z, max.z}}) {
        const uint64_t count = static_cast<uint64_t>(
            static_cast<int64_t>(high) - low + 1);
        if (count > limit || result > limit / count) return std::nullopt;
        result *= static_cast<size_t>(count);
    }
    return result;
}

struct LoopbackReplica::State {
    std::shared_ptr<const ContentDictionary> content;
    Voxel::WorldId world = Voxel::kDefaultWorldId;
    std::string zone;
    CellBounds interest;
    size_t queueLimit = 0;
    size_t cellLimit = 0;
    size_t changeLimit = 0;
    size_t byteLimit = 0;
    size_t queueBytes = 0;
    size_t cellBytes = 0;
    std::vector<std::shared_ptr<const PublicationMessage>> queue;
    std::vector<PublishedCell> cells;
    Revision revision = 0;
    Tick tick = 0;
    bool hasBaseline = false;
    bool gap = false;

    static std::optional<size_t> retainedBytes(
        const std::vector<PublishedCell>& value
    ) {
        size_t total = 0;
        if (!addElements(total, value.capacity(), sizeof(PublishedCell))) {
            return std::nullopt;
        }
        for (const auto& cell : value) {
            if (!addBytes(total, cell.state.blockKey.capacity())) {
                return std::nullopt;
            }
        }
        return total;
    }

    static std::optional<size_t> retainedBytes(
        const PublicationMessage& message
    ) {
        size_t total = sizeof(PublicationMessage);
        const bool valid = std::visit([&](const auto& value) {
            if (!addBytes(total, value.zone.capacity())) return false;
            using T = std::decay_t<decltype(value)>;
            const auto& cells = [&]() -> const std::vector<PublishedCell>& {
                if constexpr (std::is_same_v<T, WorldBaseline>) {
                    return value.cells;
                } else {
                    return value.changes;
                }
            }();
            const auto cellBytes = retainedBytes(cells);
            if (!cellBytes || !addBytes(total, *cellBytes)) return false;
            if constexpr (std::is_same_v<T, WorldChangeBatch>) {
                if (!addElements(
                        total, value.outcomes.capacity(), sizeof(CommandOutcome))) {
                    return false;
                }
            }
            return true;
        }, message);
        return valid ? std::optional<size_t>(total) : std::nullopt;
    }

    void clearQueue() {
        queue.clear();
        queueBytes = 0;
    }

    void markGap() {
        gap = true;
        clearQueue();
        cells.clear();
        cellBytes = 0;
        hasBaseline = false;
    }

    ReplicaAcceptStatus enqueue(
        std::shared_ptr<const PublicationMessage> message
    ) {
        if (!message) return ReplicaAcceptStatus::RejectedOversized;
        const bool structurallyOversized = std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const auto& cells = [&]() -> const std::vector<PublishedCell>& {
                if constexpr (std::is_same_v<T, WorldBaseline>) {
                    return value.cells;
                } else {
                    return value.changes;
                }
            }();
            if (cells.size() > (std::is_same_v<T, WorldBaseline>
                                    ? cellLimit : changeLimit)) {
                return true;
            }
            for (const auto& cell : cells) {
                if (!content->contains(cell.state.blockKey)) return true;
            }
            if constexpr (std::is_same_v<T, WorldChangeBatch>) {
                return value.outcomes.size() > changeLimit;
            }
            return false;
        }, *message);
        const auto bytes = retainedBytes(*message);
        size_t aggregate = cellBytes;
        if (structurallyOversized || !bytes || *bytes > byteLimit ||
            !addBytes(aggregate, queueBytes) ||
            !addBytes(aggregate, bytes.value_or(0)) || aggregate > byteLimit) {
            markGap();
            return ReplicaAcceptStatus::RejectedOversized;
        }
        if (gap || queue.size() >= queueLimit) {
            markGap();
            return ReplicaAcceptStatus::QueueFull;
        }
        queue.push_back(std::move(message));
        queueBytes += *bytes;
        return ReplicaAcceptStatus::Queued;
    }
};

ReplicaAcceptStatus LoopbackReplica::accept(
    std::shared_ptr<const PublicationMessage> message
) {
    return m_state->enqueue(std::move(message));
}

ReplicaPumpStatus LoopbackReplica::pumpOne() {
    if (m_state->gap) return ReplicaPumpStatus::NeedsResnapshot;
    if (m_state->queue.empty()) return ReplicaPumpStatus::Idle;
    auto message = std::move(m_state->queue.front());
    m_state->queue.erase(m_state->queue.begin());
    const auto messageBytes = LoopbackReplica::State::retainedBytes(*message);
    if (!messageBytes || *messageBytes > m_state->queueBytes) {
        m_state->markGap();
        return ReplicaPumpStatus::NeedsResnapshot;
    }
    m_state->queueBytes -= *messageBytes;

    auto fail = [&] {
        m_state->markGap();
        return ReplicaPumpStatus::NeedsResnapshot;
    };
    try {
        return std::visit([&](const auto& value) -> ReplicaPumpStatus {
        using T = std::decay_t<decltype(value)>;
        if (value.content != m_state->content->identity() ||
            value.world != m_state->world || value.zone != m_state->zone ||
            !value.complete) return fail();

        if constexpr (std::is_same_v<T, WorldBaseline>) {
            const auto volume = value.bounds.volume(m_state->cellLimit);
            if (!volume || value.bounds != m_state->interest ||
                value.cells.size() != *volume) return fail();
            const auto projectedBytes =
                LoopbackReplica::State::retainedBytes(value.cells);
            size_t projectedTransient = m_state->cellBytes;
            if (!projectedBytes ||
                !addBytes(projectedTransient, m_state->queueBytes) ||
                !addBytes(projectedTransient, *messageBytes) ||
                !addBytes(projectedTransient, *projectedBytes) ||
                projectedTransient > m_state->byteLimit) return fail();
            std::vector<PublishedCell> next = value.cells;
            std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
                return a.address < b.address;
            });
            for (size_t index = 0; index < next.size(); ++index) {
                if (!value.bounds.contains(next[index].address) ||
                    !m_state->content->supportsState(next[index].state) ||
                    (index && next[index - 1].address == next[index].address)) {
                    return fail();
                }
            }
            if (m_state->hasBaseline && value.revision == m_state->revision) {
                return ReplicaPumpStatus::Duplicate;
            }
            if (m_state->hasBaseline && value.revision < m_state->revision) {
                return fail();
            }
            const auto nextBytes = LoopbackReplica::State::retainedBytes(next);
            size_t transient = m_state->cellBytes;
            if (!nextBytes || !addBytes(transient, m_state->queueBytes) ||
                !addBytes(transient, *messageBytes) ||
                !addBytes(transient, *nextBytes) ||
                transient > m_state->byteLimit) return fail();
            m_state->cells.swap(next);
            m_state->cellBytes = *nextBytes;
            m_state->revision = value.revision;
            m_state->tick = value.tick;
            m_state->hasBaseline = true;
            return ReplicaPumpStatus::Applied;
        } else {
            if (!m_state->hasBaseline) return fail();
            if (value.revision <= m_state->revision) {
                return ReplicaPumpStatus::Duplicate;
            }
            if (value.baseRevision != m_state->revision ||
                value.revision != value.baseRevision + 1 ||
                value.tick < m_state->tick) return fail();
            size_t projectedTransient = m_state->cellBytes;
            if (!addBytes(projectedTransient, m_state->queueBytes) ||
                !addBytes(projectedTransient, *messageBytes) ||
                !addBytes(projectedTransient, m_state->cellBytes) ||
                projectedTransient > m_state->byteLimit) return fail();
            std::vector<PublishedCell> next = m_state->cells;
            std::vector<CellAddress> seen;
            seen.reserve(value.changes.size());
            for (const auto& change : value.changes) {
                if (!m_state->content->supportsState(change.state) ||
                    std::find(seen.begin(), seen.end(), change.address) != seen.end()) {
                    return fail();
                }
                seen.push_back(change.address);
                if (!m_state->interest.contains(change.address)) continue;
                const auto found = std::lower_bound(
                    next.begin(), next.end(), change.address,
                    [](const PublishedCell& cell, CellAddress address) {
                        return cell.address < address;
                    });
                if (found == next.end() || found->address != change.address) {
                    return fail();
                }
                found->state = change.state;
            }
            const auto nextBytes = LoopbackReplica::State::retainedBytes(next);
            size_t transient = m_state->cellBytes;
            if (!nextBytes || !addBytes(transient, m_state->queueBytes) ||
                !addBytes(transient, *messageBytes) ||
                !addBytes(transient, *nextBytes) ||
                transient > m_state->byteLimit) return fail();
            m_state->cells.swap(next);
            m_state->cellBytes = *nextBytes;
            m_state->revision = value.revision;
            m_state->tick = value.tick;
            return ReplicaPumpStatus::Applied;
        }
        }, *message);
    } catch (...) {
        return fail();
    }
}

ExactBlockRead LoopbackReplica::read(CellAddress address) const {
    if (!m_state->interest.contains(address)) {
        return {.status = ExactReadStatus::OutsideDomain};
    }
    if (!m_state->hasBaseline || m_state->gap) {
        return {.status = ExactReadStatus::Unavailable};
    }
    const auto found = std::lower_bound(
        m_state->cells.begin(), m_state->cells.end(), address,
        [](const PublishedCell& cell, CellAddress target) {
            return cell.address < target;
        });
    if (found == m_state->cells.end() || found->address != address) {
        return {.status = ExactReadStatus::Unavailable};
    }
    return {.status = ExactReadStatus::Known, .state = found->state};
}

bool LoopbackReplica::needsResnapshot() const { return m_state->gap; }
Revision LoopbackReplica::revision() const { return m_state->revision; }
Tick LoopbackReplica::tick() const { return m_state->tick; }
size_t LoopbackReplica::queuedMessages() const { return m_state->queue.size(); }

struct SimulationHost::Impl {
    struct Receipt {
        EditCommand command;
        uint64_t admission = 0;
        std::optional<CommandOutcome> outcome;
        bool pending = true;
    };

    std::unique_ptr<Voxel::World> world;
    std::vector<Receipt> receipts;
    std::vector<std::weak_ptr<LoopbackReplica::State>> replicas;
    SessionId currentSession = 0;
    SessionId sessionHighWater = 0;
    Entity::EntityId sessionActor;
    uint64_t nextAdmission = 1;
};

SimulationHost::SimulationHost(
    Voxel::WorldResources& resources,
    std::shared_ptr<const Voxel::WorldGenerator> generator,
    SimulationHostConfig config
) : m_impl(std::make_unique<Impl>()), m_config(std::move(config)) {
    if (!generator || !resources.registry().frozen() ||
        m_config.zone.empty() || !m_config.domain.volume(m_config.maxSnapshotCells) ||
        m_config.tickRate.numerator == 0 || m_config.tickRate.denominator == 0 ||
        m_config.maxCatchUpTicks == 0 || m_config.maxPendingCommands == 0 ||
        m_config.maxSessionReceipts == 0 || m_config.maxReplicaQueue == 0 ||
        m_config.maxEntities == 0 ||
        m_config.maxChangesPerCommand == 0 || m_config.maxCommandBytes == 0 ||
        m_config.maxReplicaBytes == 0 ||
        !std::isfinite(m_config.maxInteractionDistance) ||
        m_config.maxInteractionDistance <= 0.0f) {
        throw std::invalid_argument("invalid bounded simulation host configuration");
    }
    const uint64_t threshold =
        uint64_t{m_config.tickRate.denominator} * 1'000'000'000ULL;
    if (threshold / 1'000'000'000ULL != m_config.tickRate.denominator) {
        throw std::invalid_argument("simulation tick rate exceeds clock range");
    }

    m_content = std::make_shared<const ContentDictionary>(
        resources.registry(), *generator);
    m_impl->world = std::make_unique<Voxel::World>(resources);
    m_impl->world->setId(m_config.world);
    m_impl->world->setGenerator(generator);
    m_impl->world->requireExactCollisionCoverage({
        .min = {m_config.domain.min.x, m_config.domain.min.y,
                m_config.domain.min.z},
        .max = {m_config.domain.max.x, m_config.domain.max.y,
                m_config.domain.max.z},
    });

    std::vector<Voxel::ChunkCoord> chunks = m_config.preloadedChunks;
    if (chunks.empty()) {
        const auto first = Voxel::worldToChunk(
            m_config.domain.min.x, m_config.domain.min.y, m_config.domain.min.z);
        const auto last = Voxel::worldToChunk(
            m_config.domain.max.x, m_config.domain.max.y, m_config.domain.max.z);
        for (int64_t z = first.z; z <= last.z; ++z) {
            for (int64_t y = first.y; y <= last.y; ++y) {
                for (int64_t x = first.x; x <= last.x; ++x) {
                    if (chunks.size() >= m_config.maxPreloadedChunks) {
                        throw std::invalid_argument("preloaded terrain exceeds chunk cap");
                    }
                    chunks.push_back({static_cast<int32_t>(x),
                                      static_cast<int32_t>(y),
                                      static_cast<int32_t>(z)});
                }
            }
        }
    }
    std::sort(chunks.begin(), chunks.end());
    chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
    if (chunks.size() > m_config.maxPreloadedChunks) {
        throw std::invalid_argument("preloaded terrain exceeds chunk cap");
    }
    for (const auto chunkCoord : chunks) {
        Voxel::ChunkBuffer generated;
        generator->generate(chunkCoord, generated);
        m_impl->world->chunkManager().getOrCreateChunk(chunkCoord).copyFrom(
            generated.blocks, resources.registry());
    }

    m_impl->receipts.reserve(m_config.maxSessionReceipts);
    m_impl->replicas.reserve(m_config.maxReplicas);
}

SimulationHost::~SimulationHost() = default;

const Voxel::World& SimulationHost::world() const { return *m_impl->world; }

ExactBlockRead SimulationHost::read(CellAddress address) const {
    if (!m_config.domain.contains(address)) {
        return {.status = ExactReadStatus::OutsideDomain};
    }
    const auto chunkCoord = Voxel::worldToChunk(address.x, address.y, address.z);
    const Voxel::Chunk* chunk = m_impl->world->chunkManager().getChunk(chunkCoord);
    if (!chunk) return {.status = ExactReadStatus::Unavailable};
    int x = 0, y = 0, z = 0;
    Voxel::worldToLocal(address.x, address.y, address.z, x, y, z);
    try {
        return {
            .status = ExactReadStatus::Known,
            .state = m_content->semanticState(chunk->getBlock(x, y, z)),
        };
    } catch (const ContentManifestError&) {
        return {.status = ExactReadStatus::InvalidState};
    }
}

Entity::EntityId SimulationHost::spawnEntity(
    std::unique_ptr<Entity::Entity> entity
) {
    if (!entity || m_nextEntityId == 0 || !entity->id().isNull() ||
        m_impl->world->entities().size() >= m_config.maxEntities ||
        entity->tags().size() > m_config.maxEntityTags ||
        !entity->tags().retainedStringsFit(m_config.maxEntityTagBytes) ||
        !m_content->supportsEntity(*entity) || !finite(entity->position()) ||
        !finite(entity->velocity()) || !finite(entity->viewDirection())) {
        return Entity::EntityId::Null();
    }
    entity->setId({1, m_config.world, m_nextEntityId++});
    return m_impl->world->entities().spawn(std::move(entity));
}

bool SimulationHost::despawnEntity(Entity::EntityId entity) {
    return m_impl->world->entities().despawn(entity);
}

SessionStartStatus SimulationHost::startSession(
    SessionId session,
    Entity::EntityId actor,
    const ContentManifestId& content
) {
    if (content != m_content->identity()) return SessionStartStatus::ContentMismatch;
    if (session == 0 || session <= m_impl->sessionHighWater) {
        return SessionStartStatus::OldSession;
    }
    if (std::any_of(m_impl->receipts.begin(), m_impl->receipts.end(),
                    [](const auto& receipt) { return receipt.pending; })) {
        return SessionStartStatus::Busy;
    }
    if (actor.isNull() || !m_impl->world->entities().get(actor)) {
        return SessionStartStatus::ActorUnavailable;
    }
    m_impl->receipts.clear();
    m_impl->currentSession = session;
    m_impl->sessionHighWater = session;
    m_impl->sessionActor = actor;
    return SessionStartStatus::Started;
}

SubmitResult SimulationHost::submit(EditCommand command) {
    return submit(std::move(command), false);
}

SubmitResult SimulationHost::submit(
    EditCommand command,
    const AuthorityEditCapability& capability
) {
    if (capability.m_owner.lock() != m_authorityEditKey) {
        return {.status = SubmitStatus::InvalidRequest};
    }
    return submit(std::move(command), true);
}

SubmitResult SimulationHost::submit(EditCommand command, bool privileged) {
    if (command.session != m_impl->currentSession || command.session == 0) {
        return {.status = SubmitStatus::OldSession};
    }
    const bool authorizedAction = privileged
        ? command.action == EditAction::Atomic
        : (command.action == EditAction::Remove ||
           command.action == EditAction::Place);
    if (!authorizedAction) {
        return {.status = SubmitStatus::InvalidRequest};
    }
    const auto duplicate = std::find_if(
        m_impl->receipts.begin(), m_impl->receipts.end(),
        [&](const auto& receipt) { return receipt.command.command == command.command; });
    if (duplicate != m_impl->receipts.end()) {
        if (duplicate->command != command) {
            return {.status = SubmitStatus::PayloadConflict};
        }
        return {
            .status = duplicate->pending ? SubmitStatus::DuplicatePending
                                         : SubmitStatus::DuplicateComplete,
            .outcome = duplicate->outcome,
        };
    }
    if (command.content != m_content->identity()) {
        return {.status = SubmitStatus::ContentMismatch};
    }
    if (command.world != m_config.world || command.zone != m_config.zone ||
        command.actor != m_impl->sessionActor) {
        return {.status = SubmitStatus::WrongDomain};
    }
    if (command.command == 0 || command.mutations.empty() ||
        command.mutations.size() > m_config.maxChangesPerCommand ||
        ((command.action == EditAction::Atomic) == command.interaction.has_value()) ||
        (command.action != EditAction::Atomic && command.mutations.size() != 1)) {
        return {.status = SubmitStatus::InvalidRequest};
    }
    if (command.interaction &&
        (!finite(command.interaction->origin) ||
         !finite(command.interaction->direction) ||
         !std::isfinite(command.interaction->maxDistance))) {
        return {.status = SubmitStatus::InvalidRequest};
    }
    for (const auto& mutation : command.mutations) {
        if (!m_content->contains(mutation.expected.blockKey) ||
            !m_content->contains(mutation.replacement.blockKey)) {
            return {.status = SubmitStatus::ContentMismatch};
        }
        if (!m_content->supportsState(mutation.expected) ||
            !m_content->supportsState(mutation.replacement)) {
            return {.status = SubmitStatus::InvalidRequest};
        }
    }
    if (command.interaction) {
        if (!m_content->contains(
                command.interaction->expectedTargetState.blockKey)) {
            return {.status = SubmitStatus::ContentMismatch};
        }
        if (!m_content->supportsState(
                command.interaction->expectedTargetState)) {
            return {.status = SubmitStatus::InvalidRequest};
        }
    }
    if (m_impl->receipts.size() >= m_config.maxSessionReceipts) {
        return {.status = SubmitStatus::ReceiptCapacity};
    }
    const size_t pending = static_cast<size_t>(std::count_if(
        m_impl->receipts.begin(), m_impl->receipts.end(),
        [](const auto& receipt) { return receipt.pending; }));
    if (pending >= m_config.maxPendingCommands) {
        return {.status = SubmitStatus::CommandCapacity};
    }
    const auto retainedBytes = commandRetainedBytes(command);
    if (!retainedBytes || *retainedBytes > m_config.maxCommandBytes) {
        return {.status = SubmitStatus::CommandCapacity};
    }
    m_impl->receipts.push_back({
        .command = std::move(command),
        .admission = m_impl->nextAdmission++,
    });
    return {.status = SubmitStatus::Accepted};
}

AdvanceResult SimulationHost::advance(std::chrono::nanoseconds elapsed) {
    if (elapsed.count() < 0) {
        throw std::invalid_argument("simulation elapsed time cannot be negative");
    }
    const uint64_t elapsedNs = static_cast<uint64_t>(elapsed.count());
    if (elapsedNs > (std::numeric_limits<uint64_t>::max() - m_timeDebt) /
            m_config.tickRate.numerator) {
        throw std::overflow_error("simulation time debt exhausted its range");
    }
    m_timeDebt += elapsedNs * m_config.tickRate.numerator;
    const uint64_t threshold =
        uint64_t{m_config.tickRate.denominator} * 1'000'000'000ULL;
    AdvanceResult result;
    while (m_timeDebt >= threshold &&
           result.ticksRun < m_config.maxCatchUpTicks) {
        runTick();
        m_timeDebt -= threshold;
        ++result.ticksRun;
    }
    result.timeDebtRemaining = m_timeDebt >= threshold;
    return result;
}

void SimulationHost::runTick() {
    if (m_tick == std::numeric_limits<Tick>::max() ||
        m_revision == std::numeric_limits<Revision>::max()) {
        throw std::overflow_error("simulation tick or revision exhausted");
    }
    const Tick nextTick = m_tick + 1;
    const Revision nextRevision = m_revision + 1;
    Impl::Receipt* receipt = nullptr;
    for (auto& candidate : m_impl->receipts) {
        if (candidate.pending) {
            receipt = &candidate;
            break;
        }
    }

    CommandOutcome outcome;
    std::vector<std::pair<CellAddress, Voxel::BlockState>> commits;
    WorldChangeBatch batch{
        .content = m_content->identity(),
        .world = m_config.world,
        .zone = m_config.zone,
        .baseRevision = m_revision,
        .revision = nextRevision,
        .tick = nextTick,
    };
    batch.changes.reserve(m_config.maxChangesPerCommand);
    batch.outcomes.reserve(1);

    if (receipt) {
        const EditCommand& command = receipt->command;
        outcome = {
            .session = command.session,
            .command = command.command,
            .admission = receipt->admission,
            .tick = nextTick,
            .revision = nextRevision,
            .status = CommandOutcomeStatus::Applied,
        };

        if (!m_impl->world->entities().get(command.actor)) {
            outcome.status = CommandOutcomeStatus::ActorUnavailable;
        }

        if (outcome.status == CommandOutcomeStatus::Applied && command.interaction) {
            const InteractionIntent& intent = *command.interaction;
            const Entity::Entity* actor =
                m_impl->world->entities().get(command.actor);
            const float directionLength = glm::length(intent.direction);
            constexpr float OriginTolerance = 0.0001f;
            bool validRay = actor && finite(actor->position()) &&
                finite(intent.origin) && finite(intent.direction) &&
                std::abs(intent.origin.x - actor->position().x) <= OriginTolerance &&
                std::abs(intent.origin.y - actor->position().y) <= OriginTolerance &&
                std::abs(intent.origin.z - actor->position().z) <= OriginTolerance &&
                std::isfinite(directionLength) &&
                directionLength > 0.0f && std::isfinite(intent.maxDistance) &&
                intent.maxDistance >= 0.0f &&
                intent.maxDistance <= m_config.maxInteractionDistance;
            std::optional<Voxel::BlockTarget> target;
            if (validRay) {
                const glm::vec3 direction = intent.direction / directionLength;
                const glm::vec3 endpoint =
                    intent.origin + direction * intent.maxDistance;
                if (!finite(direction) || !finite(endpoint)) {
                    validRay = false;
                    outcome.status = CommandOutcomeStatus::InvalidRequest;
                }
                CellBounds coverage;
                const auto& extents =
                    m_impl->world->blockRegistry().modelExtents();
                if (!extents) validRay = false;
                for (size_t axis = 0; axis < 3; ++axis) {
                    if (!validRay) break;
                    const double firstCell = std::floor(std::min(
                        static_cast<double>(intent.origin[axis]),
                        static_cast<double>(endpoint[axis])));
                    const double lastCell = std::floor(std::max(
                        static_cast<double>(intent.origin[axis]),
                        static_cast<double>(endpoint[axis])));
                    const double low = std::ceil(
                        firstCell - (*extents).max[axis] - 0.00001);
                    const double high = std::floor(
                        lastCell + 1.0 - (*extents).min[axis] + 0.00001);
                    if (!std::isfinite(low) || !std::isfinite(high) ||
                        low < std::numeric_limits<int>::min() ||
                        high > std::numeric_limits<int>::max()) {
                        validRay = false;
                        break;
                    }
                    component(coverage.min, axis) = static_cast<int>(low);
                    component(coverage.max, axis) = static_cast<int>(high);
                }
                if (validRay && !coverage.volume(m_config.maxSnapshotCells)) {
                    validRay = false;
                }
                if (validRay) {
                    target = Voxel::raycastBlock(
                        *m_impl->world, intent.origin, intent.direction,
                        intent.maxDistance);
                    const float relevantDistance =
                        target ? target->distance : intent.maxDistance;
                    for (int64_t x = coverage.min.x;
                         outcome.status == CommandOutcomeStatus::Applied &&
                         x <= coverage.max.x; ++x) {
                        for (int64_t y = coverage.min.y;
                             outcome.status == CommandOutcomeStatus::Applied &&
                             y <= coverage.max.y; ++y) {
                            for (int64_t z = coverage.min.z;
                                 z <= coverage.max.z; ++z) {
                                const CellAddress owner{
                                    static_cast<int>(x), static_cast<int>(y),
                                    static_cast<int>(z)};
                                const ExactBlockRead candidate = read(owner);
                                if (candidate.status == ExactReadStatus::Known) continue;
                                const glm::vec3 minimum{
                                    x + (*extents).min[0],
                                    y + (*extents).min[1],
                                    z + (*extents).min[2]};
                                const glm::vec3 maximum{
                                    x + (*extents).max[0],
                                    y + (*extents).max[1],
                                    z + (*extents).max[2]};
                                if (!Voxel::intersectRayAabb(
                                        intent.origin, direction, minimum, maximum,
                                        relevantDistance)) continue;
                                if (candidate.status == ExactReadStatus::OutsideDomain) {
                                    outcome.status = CommandOutcomeStatus::OutsideDomain;
                                } else if (candidate.status == ExactReadStatus::Unavailable) {
                                    outcome.status = CommandOutcomeStatus::Unavailable;
                                } else {
                                    outcome.status = CommandOutcomeStatus::InvalidRequest;
                                }
                            }
                        }
                    }
                }
            }
            if (!validRay && outcome.status == CommandOutcomeStatus::Applied) {
                outcome.status = CommandOutcomeStatus::InvalidRequest;
            }

            if (validRay && outcome.status == CommandOutcomeStatus::Applied) {
                if (!target || addressOf(target->block) != intent.expectedTarget ||
                    target->face != intent.expectedFace) {
                    outcome.status = CommandOutcomeStatus::TargetMismatch;
                } else {
                    try {
                        if (m_content->semanticState(target->state) !=
                            intent.expectedTargetState) {
                            outcome.status = CommandOutcomeStatus::TargetMismatch;
                        }
                    } catch (const ContentManifestError&) {
                        outcome.status = CommandOutcomeStatus::InvalidRequest;
                    }
                }
            }
        }

        if (outcome.status == CommandOutcomeStatus::Applied &&
            command.action != EditAction::Atomic) {
            const auto& mutation = command.mutations.front();
            int dx = 0, dy = 0, dz = 0;
            Voxel::directionOffset(
                command.interaction->expectedFace, dx, dy, dz);
            const CellAddress placement{
                command.interaction->expectedTarget.x + dx,
                command.interaction->expectedTarget.y + dy,
                command.interaction->expectedTarget.z + dz,
            };
            const bool invalidRemove = command.action == EditAction::Remove &&
                (mutation.address != command.interaction->expectedTarget ||
                 mutation.replacement.blockKey != "base:air");
            const bool invalidPlace = command.action == EditAction::Place &&
                (mutation.address != placement ||
                 mutation.replacement.blockKey == "base:air");
            if (invalidRemove || invalidPlace) {
                outcome.status = CommandOutcomeStatus::InvalidRequest;
            }
        }

        commits.reserve(command.mutations.size());
        if (outcome.status == CommandOutcomeStatus::Applied) {
            std::vector<CellAddress> seen;
            seen.reserve(command.mutations.size());
            bool changed = false;
            for (const auto& mutation : command.mutations) {
                if (std::find(seen.begin(), seen.end(), mutation.address) !=
                    seen.end()) {
                    outcome.status = CommandOutcomeStatus::InvalidRequest;
                    break;
                }
                seen.push_back(mutation.address);
                const ExactBlockRead current = read(mutation.address);
                if (current.status == ExactReadStatus::OutsideDomain) {
                    outcome.status = CommandOutcomeStatus::OutsideDomain;
                    break;
                }
                if (current.status == ExactReadStatus::Unavailable) {
                    outcome.status = CommandOutcomeStatus::Unavailable;
                    break;
                }
                if (current.status == ExactReadStatus::InvalidState) {
                    outcome.status = CommandOutcomeStatus::InvalidRequest;
                    break;
                }
                if (current.state != mutation.expected) {
                    outcome.status = CommandOutcomeStatus::StaleState;
                    break;
                }
                const auto local = m_content->localState(mutation.replacement);
                commits.emplace_back(mutation.address, local);
                if (current.state != mutation.replacement) {
                    changed = true;
                    batch.changes.push_back({
                        mutation.address, mutation.replacement});
                }
            }
            if (outcome.status == CommandOutcomeStatus::Applied && !changed) {
                outcome.status = CommandOutcomeStatus::NoChange;
            }
        }

        if (outcome.status == CommandOutcomeStatus::Applied &&
            command.action == EditAction::Place) {
            const Entity::Entity* actor =
                m_impl->world->entities().get(command.actor);
            const auto& mutation = command.mutations.front();
            const auto local = m_content->localState(mutation.replacement);
            const auto& shape =
                m_impl->world->blockRegistry().getType(local.id).collision;
            for (const auto& box : shape.boxes()) {
                if (overlap(actor->worldBounds(), translated(box, mutation.address))) {
                    outcome.status = CommandOutcomeStatus::PlacementCollision;
                    batch.changes.clear();
                    commits.clear();
                    break;
                }
            }
        }

        if (outcome.status != CommandOutcomeStatus::Applied) {
            batch.changes.clear();
            commits.clear();
        }

        batch.outcomes.push_back(outcome);
    }

    auto message = std::make_shared<const PublicationMessage>(std::move(batch));

    m_impl->world->entities().prepareTick();

    if (receipt && outcome.status == CommandOutcomeStatus::Applied) {
        std::vector<Voxel::Chunk*> preparedChunks;
        preparedChunks.reserve(commits.size());
        auto finishPreparedWrites = [&] {
            for (Voxel::Chunk* chunk : preparedChunks) {
                chunk->finishPreparedBlockWrites();
            }
        };
        try {
            for (const auto& [address, state] : commits) {
                auto* chunk = m_impl->world->chunkManager().getChunk(
                    Voxel::worldToChunk(address.x, address.y, address.z));
                if (std::find(preparedChunks.begin(), preparedChunks.end(), chunk) ==
                    preparedChunks.end()) {
                    preparedChunks.push_back(chunk);
                }
                int x = 0, y = 0, z = 0;
                Voxel::worldToLocal(address.x, address.y, address.z, x, y, z);
                chunk->prepareBlockWrite(x, y, z, state);
            }
            for (const auto& [address, state] : commits) {
                m_impl->world->setBlock(address.x, address.y, address.z, state);
            }
        } catch (...) {
            finishPreparedWrites();
            throw;
        }
        finishPreparedWrites();
    }

    m_impl->world->entities().tickPrepared(
        static_cast<float>(m_config.tickRate.denominator) /
        static_cast<float>(m_config.tickRate.numerator));
    m_tick = nextTick;
    m_revision = nextRevision;
    if (receipt) {
        receipt->outcome = outcome;
        receipt->pending = false;
    }

    for (auto iterator = m_impl->replicas.begin();
         iterator != m_impl->replicas.end();) {
        if (auto state = iterator->lock()) {
            state->enqueue(message);
            ++iterator;
        } else {
            iterator = m_impl->replicas.erase(iterator);
        }
    }
}

ReplicaConnection SimulationHost::connectReplica(CellBounds interest) {
    if (!boundsWithin(interest, m_config.domain) ||
        !interest.volume(m_config.maxSnapshotCells)) {
        return {.status = ReplicaConnectStatus::InvalidInterest};
    }
    m_impl->replicas.erase(std::remove_if(
        m_impl->replicas.begin(), m_impl->replicas.end(),
        [](const auto& replica) { return replica.expired(); }),
        m_impl->replicas.end());
    if (m_impl->replicas.size() >= m_config.maxReplicas) {
        return {.status = ReplicaConnectStatus::Capacity};
    }

    WorldBaseline baseline{
        .content = m_content->identity(),
        .world = m_config.world,
        .zone = m_config.zone,
        .bounds = interest,
        .tick = m_tick,
        .revision = m_revision,
    };
    baseline.cells.reserve(*interest.volume(m_config.maxSnapshotCells));
    for (int64_t x = interest.min.x; x <= interest.max.x; ++x) {
        for (int64_t y = interest.min.y; y <= interest.max.y; ++y) {
            for (int64_t z = interest.min.z; z <= interest.max.z; ++z) {
                const CellAddress address{
                    static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)};
                const ExactBlockRead cell = read(address);
                if (cell.status != ExactReadStatus::Known) {
                    return {.status = ReplicaConnectStatus::InvalidInterest};
                }
                baseline.cells.push_back({address, cell.state});
            }
        }
    }

    auto state = std::make_shared<LoopbackReplica::State>();
    state->content = m_content;
    state->world = m_config.world;
    state->zone = m_config.zone;
    state->interest = interest;
    state->queueLimit = m_config.maxReplicaQueue;
    state->cellLimit = m_config.maxSnapshotCells;
    state->changeLimit = m_config.maxChangesPerCommand;
    state->byteLimit = m_config.maxReplicaBytes;
    state->queue.reserve(m_config.maxReplicaQueue);
    state->cells.reserve(baseline.cells.size());
    if (state->enqueue(std::make_shared<const PublicationMessage>(
            std::move(baseline))) != ReplicaAcceptStatus::Queued) {
        return {.status = ReplicaConnectStatus::Capacity};
    }
    m_impl->replicas.push_back(state);
    return {
        .status = ReplicaConnectStatus::Connected,
        .replica = LoopbackReplica(std::move(state)),
    };
}

ReplicaConnectStatus SimulationHost::resnapshot(LoopbackReplica& replica) {
    const auto& state = replica.m_state;
    const bool connected = std::any_of(
        m_impl->replicas.begin(), m_impl->replicas.end(),
        [&](const auto& candidate) {
            const auto locked = candidate.lock();
            return locked && locked == state;
        });
    if (!connected) return ReplicaConnectStatus::InvalidInterest;

    state->markGap();

    WorldBaseline baseline{
        .content = m_content->identity(),
        .world = m_config.world,
        .zone = m_config.zone,
        .bounds = state->interest,
        .tick = m_tick,
        .revision = m_revision,
    };
    baseline.cells.reserve(*state->interest.volume(m_config.maxSnapshotCells));
    for (int64_t x = state->interest.min.x; x <= state->interest.max.x; ++x) {
        for (int64_t y = state->interest.min.y; y <= state->interest.max.y; ++y) {
            for (int64_t z = state->interest.min.z; z <= state->interest.max.z; ++z) {
                const CellAddress address{
                    static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)};
                const ExactBlockRead cell = read(address);
                if (cell.status != ExactReadStatus::Known) {
                    return ReplicaConnectStatus::InvalidInterest;
                }
                baseline.cells.push_back({address, cell.state});
            }
        }
    }
    auto message = std::make_shared<const PublicationMessage>(std::move(baseline));
    state->gap = false;
    if (state->enqueue(std::move(message)) != ReplicaAcceptStatus::Queued) {
        return ReplicaConnectStatus::Capacity;
    }
    return ReplicaConnectStatus::Connected;
}

} // namespace Rigel::Simulation
