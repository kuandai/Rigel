#include "Rigel/Simulation/SimulationHost.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/RayAabb.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
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

constexpr uint64_t CheckpointMagic = 0x524947454c435031ULL; // RIGELCP1
constexpr uint64_t RecordingMagic = 0x524947454c525031ULL; // RIGELRP1
constexpr uint32_t StateFormatVersion = 1;

class Encoder {
public:
    explicit Encoder(size_t limit) : m_limit(limit) {}
    void u8(uint8_t value) { bytes(&value, 1); }
    void boolean(bool value) { u8(value ? 1 : 0); }
    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) u8(value >> shift);
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void u64(uint64_t value) {
        u32(static_cast<uint32_t>(value >> 32));
        u32(static_cast<uint32_t>(value));
    }
    void floating(float value) { u32(std::bit_cast<uint32_t>(value)); }
    void string(std::string_view value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::length_error("checkpoint string exceeds format limit");
        }
        u32(static_cast<uint32_t>(value.size()));
        bytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    void blob(const std::vector<uint8_t>& value) {
        u64(value.size());
        bytes(value.data(), value.size());
    }
    void bytes(const uint8_t* source, size_t size) {
        if (size > m_limit - m_data.size()) {
            throw std::length_error("checkpoint exceeds configured byte limit");
        }
        m_data.insert(m_data.end(), source, source + size);
    }
    std::vector<uint8_t> finish() { return std::move(m_data); }
private:
    size_t m_limit;
    std::vector<uint8_t> m_data;
};

class Decoder {
public:
    explicit Decoder(const std::vector<uint8_t>& data) : m_data(data) {}
    uint8_t u8() { require(1); return m_data[m_at++]; }
    bool boolean() {
        const uint8_t value = u8();
        if (value > 1) throw std::runtime_error("invalid checkpoint boolean");
        return value != 0;
    }
    uint32_t u32() {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | u8();
        return value;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64() { return (uint64_t{u32()} << 32) | u32(); }
    float floating() { return std::bit_cast<float>(u32()); }
    std::string string(size_t limit) {
        const size_t size = u32();
        if (size > limit) throw std::runtime_error("checkpoint string exceeds limit");
        require(size);
        std::string result(
            reinterpret_cast<const char*>(m_data.data() + m_at), size);
        m_at += size;
        return result;
    }
    std::vector<uint8_t> blob(size_t limit) {
        const uint64_t size = u64();
        if (size > limit) throw std::runtime_error("checkpoint blob exceeds limit");
        require(static_cast<size_t>(size));
        std::vector<uint8_t> result(
            m_data.begin() + static_cast<std::ptrdiff_t>(m_at),
            m_data.begin() + static_cast<std::ptrdiff_t>(m_at + size));
        m_at += static_cast<size_t>(size);
        return result;
    }
    bool done() const { return m_at == m_data.size(); }
private:
    void require(size_t size) const {
        if (size > m_data.size() - m_at) {
            throw std::runtime_error("truncated checkpoint");
        }
    }
    const std::vector<uint8_t>& m_data;
    size_t m_at = 0;
};

void encodeId(Encoder& out, const Entity::EntityId& id) {
    out.u64(id.time); out.u32(id.random); out.u32(id.counter);
}

Entity::EntityId decodeId(Decoder& in) {
    return {in.u64(), in.u32(), in.u32()};
}

void encodeVec3(Encoder& out, const glm::vec3& value) {
    out.floating(value.x); out.floating(value.y); out.floating(value.z);
}

glm::vec3 decodeVec3(Decoder& in) {
    return {in.floating(), in.floating(), in.floating()};
}

void encodeSemantic(Encoder& out, const SemanticBlockState& state) {
    out.string(state.blockKey); out.u8(state.metadata); out.u8(state.lightLevel);
}

SemanticBlockState decodeSemantic(Decoder& in, size_t stringLimit) {
    return {in.string(stringLimit), in.u8(), in.u8()};
}

void encodeEntityState(Encoder& out, const Entity::EntitySimulationState& state) {
    encodeId(out, state.id); out.string(state.typeId);
    encodeVec3(out, state.position); encodeVec3(out, state.velocity);
    encodeVec3(out, state.acceleration); encodeVec3(out, state.viewDirection);
    out.floating(state.gravityModifier); out.boolean(state.onGround);
    out.boolean(state.collidedX); out.boolean(state.collidedY);
    out.boolean(state.collidedZ); out.floating(state.floorFriction);
    encodeVec3(out, state.localBounds.min); encodeVec3(out, state.localBounds.max);
    out.u32(static_cast<uint32_t>(state.tags.size()));
    for (const auto& tag : state.tags) out.string(tag);
    out.string(state.modelIdentifier);
    out.floating(state.renderTint.x); out.floating(state.renderTint.y);
    out.floating(state.renderTint.z); out.floating(state.renderTint.w);
}

Entity::EntitySimulationState decodeEntityState(
    Decoder& in, size_t tagLimit, size_t stringLimit
) {
    Entity::EntitySimulationState state;
    state.id = decodeId(in); state.typeId = in.string(stringLimit);
    state.position = decodeVec3(in); state.velocity = decodeVec3(in);
    state.acceleration = decodeVec3(in); state.viewDirection = decodeVec3(in);
    state.gravityModifier = in.floating(); state.onGround = in.boolean();
    state.collidedX = in.boolean(); state.collidedY = in.boolean();
    state.collidedZ = in.boolean(); state.floorFriction = in.floating();
    state.localBounds = {decodeVec3(in), decodeVec3(in)};
    const size_t tags = in.u32();
    if (tags > tagLimit) throw std::runtime_error("checkpoint entity tag cap exceeded");
    state.tags.reserve(tags);
    for (size_t i = 0; i < tags; ++i) state.tags.push_back(in.string(stringLimit));
    state.modelIdentifier = in.string(stringLimit);
    state.renderTint = {in.floating(), in.floating(), in.floating(), in.floating()};
    return state;
}

void encodeIntent(Encoder& out, const InteractionIntent& value) {
    encodeVec3(out, value.origin); encodeVec3(out, value.direction);
    out.floating(value.maxDistance);
    out.i32(value.expectedTarget.x); out.i32(value.expectedTarget.y);
    out.i32(value.expectedTarget.z); out.u8(static_cast<uint8_t>(value.expectedFace));
    encodeSemantic(out, value.expectedTargetState);
}

void encodeCommand(Encoder& out, const EditCommand& command) {
    out.u64(command.session); out.u64(command.command); encodeId(out, command.actor);
    out.u32(command.world); out.string(command.zone);
    out.bytes(command.content.bytes().data(), command.content.bytes().size());
    out.u8(static_cast<uint8_t>(command.action));
    out.boolean(command.interaction.has_value());
    if (command.interaction) encodeIntent(out, *command.interaction);
    out.u32(static_cast<uint32_t>(command.mutations.size()));
    for (const auto& mutation : command.mutations) {
        out.i32(mutation.address.x); out.i32(mutation.address.y);
        out.i32(mutation.address.z); encodeSemantic(out, mutation.expected);
        encodeSemantic(out, mutation.replacement);
    }
}

ContentManifestId decodeManifest(Decoder& in) {
    std::array<uint8_t, 32> bytes{};
    for (auto& byte : bytes) byte = in.u8();
    return ContentManifestId(bytes);
}

EditCommand decodeCommand(Decoder& in, size_t changes, size_t stringLimit) {
    EditCommand command;
    command.session = in.u64(); command.command = in.u64();
    command.actor = decodeId(in); command.world = in.u32();
    command.zone = in.string(stringLimit); command.content = decodeManifest(in);
    command.action = static_cast<EditAction>(in.u8());
    if (in.boolean()) {
        InteractionIntent intent;
        intent.origin = decodeVec3(in); intent.direction = decodeVec3(in);
        intent.maxDistance = in.floating();
        intent.expectedTarget = {in.i32(), in.i32(), in.i32()};
        intent.expectedFace = static_cast<Voxel::Direction>(in.u8());
        intent.expectedTargetState = decodeSemantic(in, stringLimit);
        command.interaction = std::move(intent);
    }
    const size_t count = in.u32();
    if (count > changes) throw std::runtime_error("checkpoint mutation cap exceeded");
    command.mutations.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        CellMutation mutation;
        mutation.address = {in.i32(), in.i32(), in.i32()};
        mutation.expected = decodeSemantic(in, stringLimit);
        mutation.replacement = decodeSemantic(in, stringLimit);
        command.mutations.push_back(std::move(mutation));
    }
    return command;
}

uint64_t stableHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool validOutcomeStatus(CommandOutcomeStatus status) {
    switch (status) {
        case CommandOutcomeStatus::Applied:
        case CommandOutcomeStatus::NoChange:
        case CommandOutcomeStatus::StaleState:
        case CommandOutcomeStatus::TargetMismatch:
        case CommandOutcomeStatus::Unavailable:
        case CommandOutcomeStatus::OutsideDomain:
        case CommandOutcomeStatus::InvalidRequest:
        case CommandOutcomeStatus::ActorUnavailable:
        case CommandOutcomeStatus::PlacementCollision:
            return true;
    }
    return false;
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
    std::vector<CommandOutcome> outcomes;
    Revision revision = 0;
    Tick tick = 0;
    bool hasBaseline = false;
    bool gap = false;

    static std::optional<size_t> conservativeBaselineBytes(
        const ContentDictionary& content,
        size_t zoneBytes,
        size_t cellCount,
        size_t queueLimit
    ) {
        size_t maximumKeyBytes = 1;
        for (const auto& entry : content.entries()) {
            if (entry.blockKey.size() == std::numeric_limits<size_t>::max()) {
                return std::nullopt;
            }
            maximumKeyBytes = std::max(
                maximumKeyBytes, entry.blockKey.size() + 1);
        }
        if (zoneBytes == std::numeric_limits<size_t>::max()) {
            return std::nullopt;
        }

        size_t cellsBytes = 0;
        if (!addElements(cellsBytes, cellCount, sizeof(PublishedCell)) ||
            !addElements(cellsBytes, cellCount, maximumKeyBytes)) {
            return std::nullopt;
        }

        size_t total = sizeof(State);
        const auto dictionaryBytes = content.retainedStorageBytes();
        if (!dictionaryBytes || !addBytes(total, *dictionaryBytes) ||
            !addBytes(total, zoneBytes + 1) ||
            !addElements(
                total, queueLimit,
                sizeof(std::shared_ptr<const PublicationMessage>)) ||
            !addBytes(total, sizeof(PublicationMessage)) ||
            !addBytes(total, zoneBytes + 1) ||
            !addBytes(total, cellsBytes) ||
            !addBytes(total, cellsBytes)) {
            return std::nullopt;
        }
        return total;
    }

    std::optional<size_t> retainedStorageBytes() const {
        size_t total = sizeof(State);
        const auto dictionaryBytes = content
            ? content->retainedStorageBytes() : std::nullopt;
        if (!dictionaryBytes || !addBytes(total, *dictionaryBytes) ||
            !addBytes(total, zone.capacity()) ||
            !addElements(
                total, queue.capacity(),
                sizeof(std::shared_ptr<const PublicationMessage>)) ||
            !addElements(
                total, outcomes.capacity(), sizeof(CommandOutcome))) {
            return std::nullopt;
        }
        return total;
    }

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
        std::vector<PublishedCell>().swap(cells);
        std::vector<CommandOutcome>().swap(outcomes);
        cellBytes = 0;
        hasBaseline = false;
    }

    ReplicaAcceptStatus checkEnqueue(const PublicationMessage* message) {
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
        const auto storageBytes = retainedStorageBytes();
        size_t aggregate = storageBytes.value_or(0);
        if (structurallyOversized || !storageBytes || !bytes ||
            *bytes > byteLimit ||
            !addBytes(aggregate, cellBytes) ||
            !addBytes(aggregate, queueBytes) ||
            !addBytes(aggregate, bytes.value_or(0)) || aggregate > byteLimit) {
            markGap();
            return ReplicaAcceptStatus::RejectedOversized;
        }
        if (gap || queue.size() >= queueLimit) {
            markGap();
            return ReplicaAcceptStatus::QueueFull;
        }
        return ReplicaAcceptStatus::Queued;
    }

    // Only the authority and the copying public ingress create these objects.
    ReplicaAcceptStatus enqueue(std::shared_ptr<const PublicationMessage> message) {
        const auto status = checkEnqueue(message.get());
        if (status != ReplicaAcceptStatus::Queued) return status;
        const auto bytes = retainedBytes(*message);
        queue.push_back(std::move(message));
        queueBytes += *bytes;
        return ReplicaAcceptStatus::Queued;
    }
};

ReplicaAcceptStatus LoopbackReplica::accept(
    std::shared_ptr<const PublicationMessage> message
) {
    const auto status = m_state->checkEnqueue(message.get());
    if (status != ReplicaAcceptStatus::Queued) return status;
    try {
        // const on a shared_ptr target does not freeze a caller's mutable alias.
        // Validate the complete source size before copying, then retain only an
        // object constructed const with its own strings and containers.
        return m_state->enqueue(
            std::make_shared<const PublicationMessage>(*message));
    } catch (const std::bad_alloc&) {
        m_state->markGap();
        return ReplicaAcceptStatus::RejectedOversized;
    }
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
            const auto storageBytes = m_state->retainedStorageBytes();
            size_t projectedTransient = storageBytes.value_or(0);
            if (!storageBytes || !projectedBytes ||
                !addBytes(projectedTransient, m_state->cellBytes) ||
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
            if (m_state->hasBaseline && value.tick <= m_state->tick) {
                return fail();
            }
            const auto nextBytes = LoopbackReplica::State::retainedBytes(next);
            size_t transient = storageBytes.value_or(0);
            if (!storageBytes || !nextBytes ||
                !addBytes(transient, m_state->cellBytes) ||
                !addBytes(transient, m_state->queueBytes) ||
                !addBytes(transient, *messageBytes) ||
                !addBytes(transient, *nextBytes) ||
                transient > m_state->byteLimit) return fail();
            m_state->cells.swap(next);
            std::vector<CommandOutcome>().swap(m_state->outcomes);
            m_state->cellBytes = *nextBytes;
            m_state->revision = value.revision;
            m_state->tick = value.tick;
            m_state->hasBaseline = true;
            return ReplicaPumpStatus::Applied;
        } else {
            // Validate a publication's own structure before classifying an
            // already-applied revision. Redelivery is harmless, malformed
            // history is not a valid duplicate.
            if (!m_state->hasBaseline || value.revision == 0 ||
                value.baseRevision != value.revision - 1 || value.tick == 0) {
                return fail();
            }
            for (size_t index = 0; index < value.changes.size(); ++index) {
                const auto& change = value.changes[index];
                if (!m_state->content->supportsState(change.state) ||
                    std::any_of(value.changes.begin(), value.changes.begin() + index,
                        [&](const PublishedCell& earlier) {
                            return earlier.address == change.address;
                        })) return fail();
            }
            for (size_t index = 0; index < value.outcomes.size(); ++index) {
                const auto& outcome = value.outcomes[index];
                if (outcome.session == 0 || outcome.command == 0 ||
                    outcome.admission == 0 || outcome.tick != value.tick ||
                    outcome.revision != value.revision ||
                    !validOutcomeStatus(outcome.status)) {
                    return fail();
                }
                const auto duplicate = [&](const CommandOutcome& other) {
                    return other.session == outcome.session &&
                        other.command == outcome.command;
                };
                if (std::any_of(
                        value.outcomes.begin(), value.outcomes.begin() + index,
                        duplicate)) {
                    return fail();
                }
            }
            if (value.revision <= m_state->revision) {
                return ReplicaPumpStatus::Duplicate;
            }
            if (value.baseRevision != m_state->revision ||
                value.revision != value.baseRevision + 1 ||
                value.tick <= m_state->tick) return fail();
            for (const auto& outcome : value.outcomes) {
                if (std::any_of(m_state->outcomes.begin(), m_state->outcomes.end(),
                    [&](const CommandOutcome& earlier) {
                        return earlier.session == outcome.session &&
                            earlier.command == outcome.command;
                    })) return fail();
            }
            const auto storageBytes = m_state->retainedStorageBytes();
            size_t projectedTransient = storageBytes.value_or(0);
            size_t projectedOutcomeCount = m_state->outcomes.size();
            if (!storageBytes ||
                !addBytes(projectedOutcomeCount, value.outcomes.size()) ||
                !addBytes(projectedTransient, m_state->cellBytes) ||
                !addBytes(projectedTransient, m_state->queueBytes) ||
                !addBytes(projectedTransient, *messageBytes) ||
                !addBytes(projectedTransient, m_state->cellBytes) ||
                !addElements(
                    projectedTransient,
                    projectedOutcomeCount,
                    sizeof(CommandOutcome)) ||
                projectedTransient > m_state->byteLimit) return fail();
            std::vector<PublishedCell> next = m_state->cells;
            std::vector<CommandOutcome> nextOutcomes;
            nextOutcomes.reserve(projectedOutcomeCount);
            nextOutcomes.insert(nextOutcomes.end(),
                m_state->outcomes.begin(), m_state->outcomes.end());
            nextOutcomes.insert(
                nextOutcomes.end(), value.outcomes.begin(), value.outcomes.end());
            for (const auto& change : value.changes) {
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
            size_t nextOutcomeBytes = 0;
            if (!addElements(
                    nextOutcomeBytes, nextOutcomes.capacity(),
                    sizeof(CommandOutcome))) {
                return fail();
            }
            size_t transient = storageBytes.value_or(0);
            if (!storageBytes || !nextBytes ||
                !addBytes(transient, m_state->cellBytes) ||
                !addBytes(transient, m_state->queueBytes) ||
                !addBytes(transient, *messageBytes) ||
                !addBytes(transient, *nextBytes) ||
                !addBytes(transient, nextOutcomeBytes) ||
                transient > m_state->byteLimit) return fail();
            m_state->cells.swap(next);
            m_state->outcomes.swap(nextOutcomes);
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

std::optional<CommandOutcome> LoopbackReplica::takeOutcome() {
    if (m_state->outcomes.empty()) return std::nullopt;
    CommandOutcome result = m_state->outcomes.front();
    m_state->outcomes.erase(m_state->outcomes.begin());
    if (m_state->outcomes.empty()) {
        std::vector<CommandOutcome>().swap(m_state->outcomes);
    }
    return result;
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

    struct SpawnAdmission { Entity::EntitySimulationState state; };
    struct DespawnAdmission { Entity::EntityId id; };
    struct SessionAdmission {
        SessionId session = 0;
        Entity::EntityId actor;
        ContentManifestId content;
    };
    struct CommandAdmission { EditCommand command; bool privileged = false; };
    using Admission = std::variant<
        SpawnAdmission, DespawnAdmission, SessionAdmission, CommandAdmission>;
    struct RecordedAdmission { Tick afterTick = 0; Admission value; };

    std::unique_ptr<Voxel::World> world;
    std::vector<Receipt> receipts;
    std::vector<std::weak_ptr<LoopbackReplica::State>> replicas;
    SessionId currentSession = 0;
    SessionId sessionHighWater = 0;
    Entity::EntityId sessionActor;
    uint64_t nextAdmission = 1;
    std::vector<uint8_t> recordingBaseline;
    std::vector<RecordedAdmission> recordingAdmissions;
    size_t recordingBytes = 0;
    bool recordingGap = false;
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
        m_config.maxReplicaBytes == 0 || m_config.maxCheckpointBytes == 0 ||
        m_config.maxReplayEvents == 0 || m_config.maxReplayBytes == 0 ||
        m_config.zone.capacity() > m_config.maxCommandBytes ||
        m_config.preloadedChunks.capacity() > m_config.maxPreloadedChunks ||
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
        resources.registry(), *generator, m_config.maxContentBytes);
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
    std::vector<Voxel::ChunkCoord>().swap(m_config.preloadedChunks);

    m_impl->receipts.reserve(m_config.maxSessionReceipts);
    m_impl->replicas.reserve(m_config.maxReplicas);
    m_impl->recordingAdmissions.reserve(m_config.maxReplayEvents);
}

std::vector<uint8_t> SimulationHost::checkpointBytes(
    uint64_t generation, uint64_t parentHash, bool includeTimeDebt
) const {
    Encoder out(m_config.maxCheckpointBytes);
    out.u64(CheckpointMagic); out.u32(StateFormatVersion);
    out.u64(generation); out.u64(parentHash);
    out.bytes(m_content->identity().bytes().data(), 32);

    std::vector<Voxel::ChunkCoord> chunks;
    m_impl->world->chunkManager().forEachChunk(
        [&](Voxel::ChunkCoord coord, const Voxel::Chunk&) { chunks.push_back(coord); });
    std::sort(chunks.begin(), chunks.end());
    if (chunks.size() > m_config.maxPreloadedChunks) {
        throw std::runtime_error("checkpoint loaded chunk coverage exceeds cap");
    }

    out.u32(m_config.world); out.string(m_config.zone);
    out.i32(m_config.domain.min.x); out.i32(m_config.domain.min.y);
    out.i32(m_config.domain.min.z); out.i32(m_config.domain.max.x);
    out.i32(m_config.domain.max.y); out.i32(m_config.domain.max.z);
    out.u32(m_config.tickRate.numerator); out.u32(m_config.tickRate.denominator);
    for (const size_t value : {
             m_config.maxPreloadedChunks, m_config.maxSnapshotCells,
             m_config.maxChangesPerCommand, m_config.maxPendingCommands,
             m_config.maxSessionReceipts, m_config.maxReplicas,
             m_config.maxReplicaQueue, m_config.maxEntities,
             m_config.maxEntityTags, m_config.maxEntityTagBytes,
             m_config.maxCommandBytes, m_config.maxContentBytes,
             m_config.maxReplicaBytes, m_config.maxCatchUpTicks,
             m_config.maxCheckpointBytes, m_config.maxReplayEvents,
             m_config.maxReplayBytes}) out.u64(value);
    out.floating(m_config.maxInteractionDistance);

    out.u32(static_cast<uint32_t>(m_content->entries().size()));
    std::vector<uint32_t> canonicalByLocal(m_content->entries().size());
    for (size_t index = 0; index < m_content->entries().size(); ++index) {
        const auto& entry = m_content->entries()[index];
        out.string(entry.blockKey);
        canonicalByLocal.at(entry.localId.type) = static_cast<uint32_t>(index);
    }

    out.u64(m_tick); out.u64(m_revision);
    out.u64(includeTimeDebt ? m_timeDebt : 0);
    out.u32(m_nextEntityId); out.u64(m_impl->nextAdmission);
    out.u32(0); // No admitted rule consumes mutable RNG state in this envelope.
    out.u64(m_impl->currentSession); out.u64(m_impl->sessionHighWater);
    encodeId(out, m_impl->sessionActor);

    out.u32(static_cast<uint32_t>(m_impl->receipts.size()));
    for (const auto& receipt : m_impl->receipts) {
        encodeCommand(out, receipt.command); out.u64(receipt.admission);
        out.boolean(receipt.pending); out.boolean(receipt.outcome.has_value());
        if (receipt.outcome) {
            const auto& value = *receipt.outcome;
            out.u64(value.session); out.u64(value.command); out.u64(value.admission);
            out.u64(value.tick); out.u64(value.revision);
            out.u8(static_cast<uint8_t>(value.status));
        }
    }

    const auto entityIds = m_impl->world->entities().sortedIds();
    if (entityIds.size() > m_config.maxEntities) {
        throw std::runtime_error("checkpoint entity cap exceeded");
    }
    out.u32(static_cast<uint32_t>(entityIds.size()));
    for (const auto id : entityIds) {
        const auto* entity = m_impl->world->entities().get(id);
        if (!entity || !m_content->supportsEntity(*entity)) {
            throw std::runtime_error("checkpoint contains unsupported entity meaning");
        }
        encodeEntityState(out, entity->simulationState());
    }

    out.u32(static_cast<uint32_t>(chunks.size()));
    std::array<Voxel::BlockState, Voxel::Chunk::VOLUME> blocks{};
    for (const auto coord : chunks) {
        out.i32(coord.x); out.i32(coord.y); out.i32(coord.z);
        const auto* chunk = m_impl->world->chunkManager().getChunk(coord);
        chunk->copyBlocks(blocks);
        for (const auto block : blocks) {
            if (block.id.type >= canonicalByLocal.size() ||
                !m_content->supportsState(m_content->semanticState(block))) {
                throw std::runtime_error("checkpoint contains unsupported block state");
            }
            out.u32(canonicalByLocal[block.id.type]);
            out.u8(block.metadata); out.u8(block.lightLevel);
        }
    }
    return out.finish();
}

bool SimulationHost::prepareRecordingBaseline() {
    if (m_impl->recordingGap) return false;
    if (!m_impl->recordingBaseline.empty()) return true;
    try {
        m_impl->recordingBaseline = checkpointBytes(0, 0);
        m_impl->recordingBytes = m_impl->recordingBaseline.size();
        if (m_impl->recordingBytes > m_config.maxReplayBytes) {
            throw std::length_error("recording baseline exceeds byte cap");
        }
        return true;
    } catch (...) {
        m_impl->recordingGap = true;
        m_impl->recordingBaseline.clear();
        return false;
    }
}

std::unique_ptr<SimulationHost> SimulationHost::restoreCheckpointBytes(
    Voxel::WorldResources& resources,
    std::shared_ptr<const Voxel::WorldGenerator> generator,
    const std::vector<uint8_t>& bytes,
    uint64_t expectedGeneration,
    uint64_t expectedParentHash
) {
    if (bytes.size() > 256ULL * 1024 * 1024) {
        throw std::runtime_error("checkpoint exceeds hard byte limit");
    }
    Decoder in(bytes);
    if (in.u64() != CheckpointMagic || in.u32() != StateFormatVersion ||
        in.u64() != expectedGeneration || in.u64() != expectedParentHash) {
        throw std::runtime_error("checkpoint envelope mismatch");
    }
    const ContentManifestId savedContent = decodeManifest(in);
    SimulationHostConfig config;
    config.world = in.u32(); config.zone = in.string(64 * 1024);
    config.domain.min = {in.i32(), in.i32(), in.i32()};
    config.domain.max = {in.i32(), in.i32(), in.i32()};
    config.tickRate = {in.u32(), in.u32()};
    size_t* limits[] = {
        &config.maxPreloadedChunks, &config.maxSnapshotCells,
        &config.maxChangesPerCommand, &config.maxPendingCommands,
        &config.maxSessionReceipts, &config.maxReplicas,
        &config.maxReplicaQueue, &config.maxEntities, &config.maxEntityTags,
        &config.maxEntityTagBytes, &config.maxCommandBytes,
        &config.maxContentBytes, &config.maxReplicaBytes,
        &config.maxCatchUpTicks, &config.maxCheckpointBytes,
        &config.maxReplayEvents, &config.maxReplayBytes};
    for (size_t* target : limits) {
        const uint64_t value = in.u64();
        if (value > std::numeric_limits<size_t>::max()) {
            throw std::runtime_error("checkpoint configuration exceeds platform range");
        }
        *target = static_cast<size_t>(value);
    }
    config.maxInteractionDistance = in.floating();
    if (bytes.size() > config.maxCheckpointBytes) {
        throw std::runtime_error("checkpoint exceeds saved byte limit");
    }

    const size_t dictionaryCount = in.u32();
    if (dictionaryCount == 0 || dictionaryCount > 65536) {
        throw std::runtime_error("checkpoint dictionary count is invalid");
    }
    std::vector<std::string> dictionary;
    dictionary.reserve(dictionaryCount);
    for (size_t i = 0; i < dictionaryCount; ++i) {
        dictionary.push_back(in.string(config.maxContentBytes));
        if (i && dictionary[i - 1] >= dictionary[i]) {
            throw std::runtime_error("checkpoint dictionary is not canonical");
        }
    }

    const Tick tick = in.u64(); const Revision revision = in.u64();
    const uint64_t timeDebt = in.u64(); const uint32_t nextEntityId = in.u32();
    const uint64_t nextAdmission = in.u64();
    if (in.u32() != 0) throw std::runtime_error("checkpoint RNG scheme is unsupported");
    const SessionId currentSession = in.u64();
    const SessionId sessionHighWater = in.u64();
    const Entity::EntityId sessionActor = decodeId(in);

    struct SavedReceipt {
        EditCommand command; uint64_t admission = 0; bool pending = false;
        std::optional<CommandOutcome> outcome;
    };
    const size_t receiptCount = in.u32();
    if (receiptCount > config.maxSessionReceipts) {
        throw std::runtime_error("checkpoint receipt cap exceeded");
    }
    std::vector<SavedReceipt> receipts;
    receipts.reserve(receiptCount);
    for (size_t i = 0; i < receiptCount; ++i) {
        SavedReceipt receipt;
        receipt.command = decodeCommand(
            in, config.maxChangesPerCommand, config.maxCommandBytes);
        receipt.admission = in.u64(); receipt.pending = in.boolean();
        if (in.boolean()) {
            CommandOutcome value;
            value.session = in.u64(); value.command = in.u64();
            value.admission = in.u64(); value.tick = in.u64();
            value.revision = in.u64();
            value.status = static_cast<CommandOutcomeStatus>(in.u8());
            if (!validOutcomeStatus(value.status)) {
                throw std::runtime_error("checkpoint outcome is invalid");
            }
            receipt.outcome = value;
        }
        if (receipt.pending == receipt.outcome.has_value()) {
            throw std::runtime_error("checkpoint receipt phase is invalid");
        }
        receipts.push_back(std::move(receipt));
    }

    const size_t entityCount = in.u32();
    if (entityCount > config.maxEntities) {
        throw std::runtime_error("checkpoint entity cap exceeded");
    }
    std::vector<Entity::EntitySimulationState> entities;
    entities.reserve(entityCount);
    for (size_t i = 0; i < entityCount; ++i) {
        entities.push_back(decodeEntityState(
            in, config.maxEntityTags, config.maxEntityTagBytes));
        if (i && !(entities[i - 1].id < entities[i].id)) {
            throw std::runtime_error("checkpoint entity order is invalid");
        }
    }

    const size_t chunkCount = in.u32();
    if (chunkCount > config.maxPreloadedChunks) {
        throw std::runtime_error("checkpoint chunk cap exceeded");
    }
    struct SavedChunk {
        Voxel::ChunkCoord coord;
        std::array<Voxel::BlockState, Voxel::Chunk::VOLUME> blocks;
    };
    std::vector<SavedChunk> chunks;
    chunks.reserve(chunkCount);
    for (size_t i = 0; i < chunkCount; ++i) {
        SavedChunk chunk{{in.i32(), in.i32(), in.i32()}, {}};
        if (i && !(chunks.back().coord < chunk.coord)) {
            throw std::runtime_error("checkpoint chunk order is invalid");
        }
        for (auto& block : chunk.blocks) {
            const uint32_t semantic = in.u32();
            if (semantic >= dictionary.size()) {
                throw std::runtime_error("checkpoint block dictionary index is invalid");
            }
            block = {Voxel::BlockID{static_cast<uint16_t>(semantic)}, in.u8(), in.u8()};
        }
        chunks.push_back(std::move(chunk));
    }
    if (!in.done()) throw std::runtime_error("checkpoint has trailing bytes");

    config.preloadedChunks.clear();
    for (const auto& chunk : chunks) config.preloadedChunks.push_back(chunk.coord);
    auto host = std::unique_ptr<SimulationHost>(
        new SimulationHost(resources, std::move(generator), config));
    host->m_content->requireIdentity(savedContent);
    if (host->m_content->entries().size() != dictionary.size()) {
        throw std::runtime_error("checkpoint semantic dictionary mismatch");
    }
    std::vector<Voxel::BlockID> localBySemantic(dictionary.size());
    for (size_t i = 0; i < dictionary.size(); ++i) {
        if (host->m_content->entries()[i].blockKey != dictionary[i]) {
            throw std::runtime_error("checkpoint semantic dictionary mismatch");
        }
        localBySemantic[i] = host->m_content->entries()[i].localId;
    }
    for (auto& chunk : chunks) {
        for (auto& block : chunk.blocks) {
            block.id = localBySemantic.at(block.id.type);
            if (!host->m_content->supportsState(host->m_content->semanticState(block))) {
                throw std::runtime_error("checkpoint block state is unsupported");
            }
        }
        host->m_impl->world->chunkManager().getChunk(chunk.coord)->copyFrom(
            chunk.blocks, resources.registry());
    }
    for (const auto& state : entities) {
        auto entity = std::make_unique<Entity::Entity>(state.typeId);
        entity->restoreSimulationState(state);
        if (!host->m_content->supportsEntity(*entity) ||
            host->m_impl->world->entities().spawn(std::move(entity)) != state.id) {
            throw std::runtime_error("checkpoint entity state is unsupported");
        }
    }
    host->m_impl->receipts.clear();
    for (auto& saved : receipts) {
        host->m_impl->receipts.push_back({
            std::move(saved.command), saved.admission,
            saved.outcome, saved.pending});
    }
    host->m_tick = tick; host->m_revision = revision;
    host->m_timeDebt = timeDebt; host->m_nextEntityId = nextEntityId;
    host->m_impl->nextAdmission = nextAdmission;
    host->m_impl->currentSession = currentSession;
    host->m_impl->sessionHighWater = sessionHighWater;
    host->m_impl->sessionActor = sessionActor;
    host->m_impl->recordingBaseline.clear();
    host->m_impl->recordingAdmissions.clear();
    return host;
}

uint64_t SimulationHost::stateHash() const {
    return stableHash(checkpointBytes(0, 0, false));
}

std::optional<SimulationRecording> SimulationHost::recording() const {
    if (m_impl->recordingGap) return std::nullopt;
    try {
        const std::vector<uint8_t> currentBaseline =
            m_impl->recordingBaseline.empty()
                ? checkpointBytes(0, 0)
                : std::vector<uint8_t>{};
        const auto& baseline = m_impl->recordingBaseline.empty()
            ? currentBaseline : m_impl->recordingBaseline;
        Encoder out(m_config.maxReplayBytes);
        out.u64(RecordingMagic); out.u32(StateFormatVersion);
        out.bytes(m_content->identity().bytes().data(), 32);
        out.u64(m_tick); const uint64_t hash = stateHash(); out.u64(hash);
        out.blob(baseline);
        out.u32(static_cast<uint32_t>(m_impl->recordingAdmissions.size()));
        for (const auto& event : m_impl->recordingAdmissions) {
            out.u64(event.afterTick);
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Impl::SpawnAdmission>) {
                    out.u8(1); encodeEntityState(out, value.state);
                } else if constexpr (std::is_same_v<T, Impl::DespawnAdmission>) {
                    out.u8(2); encodeId(out, value.id);
                } else if constexpr (std::is_same_v<T, Impl::SessionAdmission>) {
                    out.u8(3); out.u64(value.session); encodeId(out, value.actor);
                    out.bytes(value.content.bytes().data(), 32);
                } else {
                    out.u8(4); out.boolean(value.privileged);
                    encodeCommand(out, value.command);
                }
            }, event.value);
        }
        return SimulationRecording{
            .content = m_content->identity(),
            .finalTick = m_tick,
            .finalHash = hash,
            .bytes = out.finish(),
        };
    } catch (...) {
        return std::nullopt;
    }
}

ResimulationResult SimulationHost::resimulate(
    Voxel::WorldResources& resources,
    std::shared_ptr<const Voxel::WorldGenerator> generator,
    const SimulationRecording& recording,
    const std::vector<std::chrono::nanoseconds>& framePacing
) {
    ResimulationResult result;
    try {
        if (recording.bytes.size() > 256ULL * 1024 * 1024 ||
            framePacing.empty()) return result;
        Decoder in(recording.bytes);
        if (in.u64() != RecordingMagic || in.u32() != StateFormatVersion) {
            return result;
        }
        const auto content = decodeManifest(in);
        const Tick finalTick = in.u64(); const uint64_t finalHash = in.u64();
        if (content != recording.content || finalTick != recording.finalTick ||
            finalHash != recording.finalHash) return result;
        auto baseline = in.blob(256ULL * 1024 * 1024);
        auto host = restoreCheckpointBytes(resources, generator, baseline, 0, 0);
        if (host->content().identity() != content) {
            result.status = ResimulationStatus::EnvelopeMismatch;
            return result;
        }
        const size_t count = in.u32();
        if (count > host->m_config.maxReplayEvents) return result;
        std::vector<Impl::RecordedAdmission> events;
        events.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const Tick afterTick = in.u64(); const uint8_t kind = in.u8();
            if (afterTick > finalTick || (i && afterTick < events.back().afterTick)) {
                return result;
            }
            if (kind == 1) {
                events.push_back({afterTick, Impl::SpawnAdmission{
                    decodeEntityState(in, host->m_config.maxEntityTags,
                                      host->m_config.maxEntityTagBytes)}});
            } else if (kind == 2) {
                events.push_back({afterTick, Impl::DespawnAdmission{decodeId(in)}});
            } else if (kind == 3) {
                Impl::SessionAdmission value;
                value.session = in.u64(); value.actor = decodeId(in);
                value.content = decodeManifest(in);
                events.push_back({afterTick, std::move(value)});
            } else if (kind == 4) {
                const bool privileged = in.boolean();
                events.push_back({afterTick, Impl::CommandAdmission{
                    decodeCommand(in, host->m_config.maxChangesPerCommand,
                                  host->m_config.maxCommandBytes), privileged}});
            } else {
                return result;
            }
        }
        if (!in.done()) return result;

        size_t nextEvent = 0;
        auto applyDue = [&]() -> bool {
            while (nextEvent < events.size() &&
                   events[nextEvent].afterTick == host->m_tick) {
                bool accepted = std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, Impl::SpawnAdmission>) {
                        auto entity = std::make_unique<Entity::Entity>(value.state.typeId);
                        entity->restoreSimulationState(value.state);
                        entity->setId(Entity::EntityId::Null());
                        return host->spawnEntity(std::move(entity)) == value.state.id;
                    } else if constexpr (std::is_same_v<T, Impl::DespawnAdmission>) {
                        return host->despawnEntity(value.id);
                    } else if constexpr (std::is_same_v<T, Impl::SessionAdmission>) {
                        return host->startSession(
                            value.session, value.actor, value.content) ==
                            SessionStartStatus::Started;
                    } else {
                        const SubmitResult submitted = value.privileged
                            ? host->submit(value.command, host->authorityEditCapability())
                            : host->submit(value.command);
                        return submitted.status == SubmitStatus::Accepted;
                    }
                }, events[nextEvent].value);
                if (!accepted) return false;
                ++nextEvent;
            }
            return nextEvent == events.size() ||
                events[nextEvent].afterTick > host->m_tick;
        };

        for (const auto elapsed : framePacing) {
            if (elapsed.count() <= 0) return result;
        }
        size_t frame = 0;
        while (host->m_tick < finalTick) {
            const auto elapsed = framePacing[frame++ % framePacing.size()];
            const uint64_t elapsedNs = static_cast<uint64_t>(elapsed.count());
            if (elapsedNs > (std::numeric_limits<uint64_t>::max() -
                    host->m_timeDebt) / host->m_config.tickRate.numerator) {
                return result;
            }
            host->m_timeDebt += elapsedNs * host->m_config.tickRate.numerator;
            const uint64_t threshold =
                uint64_t{host->m_config.tickRate.denominator} * 1'000'000'000ULL;
            size_t ticksThisFrame = 0;
            while (host->m_timeDebt >= threshold && host->m_tick < finalTick &&
                   ticksThisFrame < host->m_config.maxCatchUpTicks) {
                if (!applyDue()) {
                    result.status = ResimulationStatus::Diverged;
                    return result;
                }
                host->runTick();
                host->m_timeDebt -= threshold;
                ++ticksThisFrame;
            }
            if (frame > 10'000'000) return result;
        }
        if (!applyDue() || nextEvent != events.size()) {
            result.status = ResimulationStatus::Diverged;
            return result;
        }
        result.tick = host->tick(); result.stateHash = host->stateHash();
        if (result.stateHash != finalHash) {
            result.status = ResimulationStatus::Diverged;
            return result;
        }
        result.status = ResimulationStatus::Complete;
        result.host = std::move(host);
        return result;
    } catch (const ContentManifestError&) {
        result.status = ResimulationStatus::EnvelopeMismatch;
        return result;
    } catch (...) {
        return result;
    }
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
    const auto tagBytes = entity
        ? entity->tags().retainedStorageBytes() : std::nullopt;
    size_t semanticBytes = tagBytes.value_or(0);
    const bool retainedStorageFits = tagBytes &&
        addBytes(semanticBytes, entity->typeId().capacity()) &&
        addBytes(semanticBytes, entity->modelIdentifier().capacity()) &&
        addBytes(semanticBytes, entity->model().id().capacity()) &&
        semanticBytes <= m_config.maxEntityTagBytes;
    if (!entity || m_nextEntityId == 0 || !entity->id().isNull() ||
        m_impl->world->entities().size() >= m_config.maxEntities ||
        entity->tags().size() > m_config.maxEntityTags ||
        !retainedStorageFits ||
        !m_content->supportsEntity(*entity) || !finite(entity->position()) ||
        !finite(entity->velocity()) || !finite(entity->acceleration()) ||
        !finite(entity->viewDirection())) {
        return Entity::EntityId::Null();
    }
    prepareRecordingBaseline();
    const uint32_t allocatedId = m_nextEntityId++;
    entity->setId({1, m_config.world, allocatedId});
    bool recorded = false;
    if (!m_impl->recordingGap) {
        try {
            if (m_impl->recordingAdmissions.size() >= m_config.maxReplayEvents) {
                throw std::length_error("recording event cap exceeded");
            }
            m_impl->recordingAdmissions.push_back({
                m_tick, Impl::SpawnAdmission{entity->simulationState()}});
            recorded = true;
        } catch (...) {
            m_impl->recordingGap = true;
            m_impl->recordingAdmissions.clear();
        }
    }
    try {
        const Entity::EntityId result =
            m_impl->world->entities().spawn(std::move(entity));
        if (!result.isNull()) return result;
    } catch (...) {
        m_nextEntityId = allocatedId;
        if (recorded) m_impl->recordingAdmissions.pop_back();
        throw;
    }
    m_nextEntityId = allocatedId;
    if (recorded) m_impl->recordingAdmissions.pop_back();
    return Entity::EntityId::Null();
}

bool SimulationHost::despawnEntity(Entity::EntityId entity) {
    if (!m_impl->world->entities().get(entity)) return false;
    prepareRecordingBaseline();
    if (!m_impl->recordingGap) {
        if (m_impl->recordingAdmissions.size() >= m_config.maxReplayEvents) {
            m_impl->recordingGap = true;
            m_impl->recordingAdmissions.clear();
        } else {
            m_impl->recordingAdmissions.push_back({
                m_tick, Impl::DespawnAdmission{entity}});
        }
    }
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
    prepareRecordingBaseline();
    if (!m_impl->recordingGap) {
        try {
            if (m_impl->recordingAdmissions.size() >= m_config.maxReplayEvents) {
                throw std::length_error("recording event cap exceeded");
            }
            m_impl->recordingAdmissions.push_back({
                m_tick, Impl::SessionAdmission{session, actor, content}});
        } catch (...) {
            m_impl->recordingGap = true;
            m_impl->recordingAdmissions.clear();
        }
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
    prepareRecordingBaseline();
    if (!m_impl->recordingGap) {
        try {
            if (m_impl->recordingAdmissions.size() >= m_config.maxReplayEvents) {
                throw std::length_error("recording event cap exceeded");
            }
            m_impl->recordingAdmissions.push_back({
                m_tick, Impl::CommandAdmission{command, privileged}});
        } catch (...) {
            m_impl->recordingGap = true;
            m_impl->recordingAdmissions.clear();
        }
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
    const auto volume = interest.volume(m_config.maxSnapshotCells);
    if (!boundsWithin(interest, m_config.domain) || !volume) {
        return {.status = ReplicaConnectStatus::InvalidInterest};
    }
    m_impl->replicas.erase(std::remove_if(
        m_impl->replicas.begin(), m_impl->replicas.end(),
        [](const auto& replica) { return replica.expired(); }),
        m_impl->replicas.end());
    if (m_impl->replicas.size() >= m_config.maxReplicas) {
        return {.status = ReplicaConnectStatus::Capacity};
    }

    const auto requiredBytes = LoopbackReplica::State::conservativeBaselineBytes(
        *m_content, m_config.zone.size(), *volume, m_config.maxReplicaQueue);
    if (!requiredBytes || *requiredBytes > m_config.maxReplicaBytes) {
        return {.status = ReplicaConnectStatus::Capacity};
    }

    try {
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
        const auto storageBytes = state->retainedStorageBytes();
        if (!storageBytes || *storageBytes > m_config.maxReplicaBytes) {
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
        baseline.cells.reserve(*volume);
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

        if (state->enqueue(std::make_shared<const PublicationMessage>(
                std::move(baseline))) != ReplicaAcceptStatus::Queued) {
            return {.status = ReplicaConnectStatus::Capacity};
        }
        m_impl->replicas.push_back(state);
        return {
            .status = ReplicaConnectStatus::Connected,
            .replica = LoopbackReplica(std::move(state)),
        };
    } catch (const std::bad_alloc&) {
        return {.status = ReplicaConnectStatus::Capacity};
    }
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
    // Recovery replaces a failed replica only. Proactive refresh of a healthy
    // subscription is unnecessary and must not discard its readable cut or
    // unread outcomes if preparing a replacement would fail.
    if (!state->gap) return ReplicaConnectStatus::NotNeeded;

    const auto volume = state->interest.volume(m_config.maxSnapshotCells);
    const auto requiredBytes = volume
        ? LoopbackReplica::State::conservativeBaselineBytes(
              *m_content, m_config.zone.size(), *volume,
              m_config.maxReplicaQueue)
        : std::nullopt;
    if (!requiredBytes || *requiredBytes > m_config.maxReplicaBytes) {
        return ReplicaConnectStatus::Capacity;
    }

    state->markGap();

    try {
        WorldBaseline baseline{
            .content = m_content->identity(),
            .world = m_config.world,
            .zone = m_config.zone,
            .bounds = state->interest,
            .tick = m_tick,
            .revision = m_revision,
        };
        baseline.cells.reserve(*volume);
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

        auto message = std::make_shared<const PublicationMessage>(
            std::move(baseline));
        state->gap = false;
        if (state->enqueue(std::move(message)) != ReplicaAcceptStatus::Queued) {
            return ReplicaConnectStatus::Capacity;
        }
        return ReplicaConnectStatus::Connected;
    } catch (const std::bad_alloc&) {
        state->markGap();
        return ReplicaConnectStatus::Capacity;
    }
}

} // namespace Rigel::Simulation
