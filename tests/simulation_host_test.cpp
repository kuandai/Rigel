#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityTags.h"
#include "Rigel/Persistence/InMemoryStorage.h"
#include "Rigel/Simulation/SimulationCheckpoint.h"
#include "Rigel/Simulation/SimulationHost.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
namespace {
bool failAllocation = false;
size_t allocationsBeforeFailure = 0;
size_t failureAllocationSize = 0;
bool trackAllocations = false;
size_t trackedAllocations = 0;
size_t trackedAllocationBytes = 0;
thread_local size_t allocationCeiling = 0;
thread_local bool allocationCeilingExceeded = false;
thread_local size_t watchedAllocationSize = 0;
thread_local bool watchedAllocationObserved = false;
}

void* operator new(std::size_t bytes) {
    if (watchedAllocationSize && bytes == watchedAllocationSize) {
        watchedAllocationObserved = true;
    }
    if (allocationCeiling && bytes > allocationCeiling) {
        allocationCeilingExceeded = true;
        throw std::bad_alloc();
    }
    if (trackAllocations) {
        ++trackedAllocations;
        trackedAllocationBytes += bytes;
    }
    if (failAllocation &&
        (failureAllocationSize == 0 || bytes == failureAllocationSize)) {
        if (allocationsBeforeFailure == 0) {
            failAllocation = false;
            throw std::bad_alloc();
        }
        --allocationsBeforeFailure;
    }
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) {
        return allocation;
    }
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

TEST_CASE(EntitySimulationState_allocation_failure_preserves_existing_state) {
    for (size_t failure = 0; failure < 16; ++failure) {
        Rigel::Entity::Entity entity;
        entity.setPosition({1, 2, 3});
        entity.addTag("retained");
        const auto before = entity.simulationState();
        auto replacement = before;
        replacement.position = {8, 9, 10};
        replacement.tags = {std::string(64, 'a'), std::string(64, 'b')};
        replacement.modelIdentifier = std::string(128, 'm');
        bool failed = false;
        failureAllocationSize = 0;
        allocationsBeforeFailure = failure;
        failAllocation = true;
        try {
            entity.restoreSimulationState(replacement);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        failAllocation = false;
        CHECK_EQ(entity.simulationState(), failed ? before : replacement);
    }
}
#endif

namespace {

using namespace Rigel;
using namespace Rigel::Simulation;
using namespace std::chrono_literals;

Voxel::GeneratorDefinitionData flatDefinition(
    std::string solid = "rigel:stone",
    std::string surface = "rigel:grass",
    std::string water = "rigel:water"
) {
    auto data = Test::generatorDefinitionFixture(
        std::move(solid), std::move(surface), std::move(water));
    data.bounds = {-31, 30};
    data.terrain.seaLevel = -10;
    data.terrain.densityOutput = "terrain";
    data.densityGraph.nodes.front().type = "y";
    data.densityGraph.nodes.front().value = 0.0f;
    data.densityGraph.nodes.front().scale = -1.0f;
    data.densityGraph.nodes.front().offset = 0.0f;
    data.densityGraph.outputs.front() = {"terrain", "ground"};
    return data;
}

struct HostFixture {
    Voxel::WorldResources resources;
    std::shared_ptr<Voxel::WorldGenerator> generator;
    std::unique_ptr<SimulationHost> host;
    Entity::EntityId actor;

    explicit HostFixture(
        SimulationHostConfig config = {},
        glm::vec3 actorPosition = {5.5f, 6.0f, 5.5f},
        glm::vec3 actorVelocity = {0.0f, 0.0f, 0.0f},
        size_t extraBlockCount = 0
    ) {
        for (const std::string identifier : {
                 "rigel:stone", "rigel:grass", "rigel:water"}) {
            Voxel::BlockType type;
            type.identifier = identifier;
            if (identifier == "rigel:water") {
                type.isOpaque = false;
                type.collision = Voxel::BlockCollisionShape::empty();
            }
            resources.registry().registerBlock(identifier, std::move(type));
        }
        Voxel::BlockModelCuboid overhang;
        overhang.bounds = {{-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
        overhang.faces[static_cast<size_t>(Voxel::Direction::PosY)] =
            Voxel::BlockModelFace{.textureSlot = "surface"};
        Voxel::BlockType overhanging;
        overhanging.identifier = "rigel:overhang";
        overhanging.model = std::make_shared<const Voxel::BlockModel>(
            "rigel:overhang_model", std::vector<std::string>{"surface"},
            std::vector<Voxel::BlockModelCuboid>{std::move(overhang)});
        overhanging.collision = Voxel::BlockCollisionShape::empty();
        resources.registry().registerBlock(
            "rigel:overhang", std::move(overhanging));
        for (size_t index = 0; index < extraBlockCount; ++index) {
            const std::string identifier =
                "rigel:dictionary_test_" + std::to_string(index);
            Voxel::BlockType type;
            type.identifier = identifier;
            resources.registry().registerBlock(identifier, std::move(type));
        }
        resources.registry().freeze();
        generator = std::make_shared<Voxel::WorldGenerator>(
            resources.registry(), flatDefinition(), 17);

        if (config.domain == CellBounds{{0, 0, 0}, {31, 31, 31}}) {
            config.domain = {{-4, -4, -4}, {20, 8, 20}};
            config.maxPreloadedChunks = 8;
            config.maxSnapshotCells = 25'000;
        }
        host = std::make_unique<SimulationHost>(
            resources, generator, std::move(config));
        auto entity = std::make_unique<Entity::Entity>();
        entity->addTag(Entity::EntityTags::NoClip);
        entity->setPosition(actorPosition);
        entity->setVelocity(actorVelocity);
        actor = host->spawnEntity(std::move(entity));
        CHECK(!actor.isNull());
    }

    CellAddress surface(int x = 5, int z = 5) const {
        for (int y = 4; y >= -3; --y) {
            const auto current = host->read({x, y, z});
            const auto above = host->read({x, y + 1, z});
            if (current.status == ExactReadStatus::Known &&
                current.state.blockKey != "base:air" &&
                above.status == ExactReadStatus::Known &&
                above.state.blockKey == "base:air") {
                return {x, y, z};
            }
        }
        throw Test::TestFailure("flat fixture has no surface");
    }

    EditCommand removeCommand(CommandId id) const {
        const CellAddress target = surface();
        const auto targetState = host->read(target).state;
        EditCommand command{
            .session = 1,
            .command = id,
            .actor = actor,
            .world = 0,
            .zone = "base:default",
            .content = host->content().identity(),
            .action = EditAction::Remove,
            .interaction = InteractionIntent{
                .origin = host->world().entities().get(actor)->position(),
                .direction = {0.0f, -1.0f, 0.0f},
                .maxDistance = 8.0f,
                .expectedTarget = target,
                .expectedFace = Voxel::Direction::PosY,
                .expectedTargetState = targetState,
            },
            .mutations = {{
                .address = target,
                .expected = targetState,
                .replacement = {"base:air", 0},
            }},
        };
        return command;
    }

    void start() {
        CHECK_EQ(
            host->startSession(1, actor, host->content().identity()),
            SessionStartStatus::Started);
    }
};

class CheckpointFaultStorage final : public Persistence::StorageBackend {
public:
    enum class Fault {
        None,
        DelayPayload,
        PayloadOpen,
        PayloadWrite,
        PayloadFlush,
        PayloadCommit,
        PayloadUncertain,
        PointerNotPublished,
        PointerUncertain,
        PendingUncertain,
        RemovePending,
    };

    explicit CheckpointFaultStorage(
        Fault fault,
        std::shared_ptr<Persistence::StorageBackend> storage =
            std::make_shared<Persistence::InMemoryStorageBackend>())
        : m_fault(fault), m_storage(std::move(storage)) {}

    class Session final : public Persistence::AtomicWriteSession,
                          public Persistence::ByteWriter {
    public:
        Session(CheckpointFaultStorage& owner, std::string path,
                std::unique_ptr<Persistence::AtomicWriteSession> delegate)
            : m_owner(owner), m_path(std::move(path)),
              m_delegate(std::move(delegate)) {}
        Persistence::ByteWriter& writer() override { return *this; }
        void writeU8(uint8_t value) override { m_delegate->writer().writeU8(value); }
        void writeU16(uint16_t value) override { m_delegate->writer().writeU16(value); }
        void writeU32(uint32_t value) override { m_delegate->writer().writeU32(value); }
        void writeI32(int32_t value) override { m_delegate->writer().writeI32(value); }
        void writeBytes(const uint8_t* data, size_t size) override {
            if (m_path.ends_with(".bin") && m_owner.takeFault(Fault::PayloadWrite)) {
                m_delegate->writer().writeBytes(data, std::min(size, size_t{32}));
                throw std::runtime_error("injected payload write failure");
            }
            m_delegate->writer().writeBytes(data, size);
        }
        size_t size() const override { return m_delegate->writer().size(); }
        size_t tell() const override { return m_delegate->writer().tell(); }
        void seek(size_t offset) override { m_delegate->writer().seek(offset); }
        void writeAt(size_t offset, const uint8_t* data, size_t size) override {
            m_delegate->writer().writeAt(offset, data, size);
        }
        void flush() override {
            if (m_path.ends_with(".bin") && m_owner.takeFault(Fault::PayloadFlush)) {
                throw std::runtime_error("injected payload flush failure");
            }
            m_delegate->writer().flush();
        }
        void commit() override {
            if (m_owner.m_fault == Fault::DelayPayload && m_path.ends_with(".bin")) {
                std::unique_lock lock(m_owner.m_mutex);
                m_owner.m_payloadWaiting = true;
                m_owner.m_changed.notify_all();
                m_owner.m_changed.wait(lock, [&] { return m_owner.m_releasePayload; });
            }
            if (m_path.ends_with(".bin") && m_owner.takeFault(Fault::PayloadCommit)) {
                throw Persistence::AtomicFilePublicationError(
                    Persistence::AtomicFilePublicationState::NotPublished,
                    "injected payload commit failure");
            }
            if (m_path.ends_with("/current") &&
                m_owner.takeFault(Fault::PointerNotPublished)) {
                throw Persistence::AtomicFilePublicationError(
                    Persistence::AtomicFilePublicationState::NotPublished,
                    "injected pre-publication failure");
            }
            m_delegate->commit();
            if (m_path.ends_with(".bin") && m_owner.takeFault(Fault::PayloadUncertain)) {
                throw Persistence::AtomicFilePublicationError(
                    Persistence::AtomicFilePublicationState::PublishedDurabilityUncertain,
                    "injected inactive payload durability uncertainty");
            }
            if (m_path.ends_with("/publication-pending") &&
                m_owner.takeFault(Fault::PendingUncertain)) {
                throw Persistence::AtomicFilePublicationError(
                    Persistence::AtomicFilePublicationState::PublishedDurabilityUncertain,
                    "injected pending-marker sync failure");
            }
            if (m_path.ends_with("/current") &&
                m_owner.takeFault(Fault::PointerUncertain)) {
                throw Persistence::AtomicFilePublicationError(
                    Persistence::AtomicFilePublicationState::PublishedDurabilityUncertain,
                    "injected post-publication sync failure");
            }
        }
        void abort() override { m_delegate->abort(); }
    private:
        CheckpointFaultStorage& m_owner;
        std::string m_path;
        std::unique_ptr<Persistence::AtomicWriteSession> m_delegate;
    };

    void waitForPayload() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return m_payloadWaiting; });
    }
    void releasePayload() {
        std::lock_guard lock(m_mutex);
        m_releasePayload = true;
        m_changed.notify_all();
    }

    bool takeFault(Fault expected) {
        std::lock_guard lock(m_mutex);
        if (m_fault != expected) return false;
        m_fault = Fault::None;
        return true;
    }

    void setFault(Fault fault) {
        std::lock_guard lock(m_mutex);
        m_fault = fault;
    }

    std::unique_ptr<Persistence::ByteReader> openRead(const std::string& path) override {
        return m_storage->openRead(path);
    }
    std::unique_ptr<Persistence::AtomicWriteSession> openWrite(
        const std::string& path) override {
        if (path.ends_with(".bin") && takeFault(Fault::PayloadOpen)) {
            throw std::runtime_error("injected payload open failure");
        }
        return std::make_unique<Session>(*this, path, m_storage->openWrite(path));
    }
    bool exists(const std::string& path) override { return m_storage->exists(path); }
    Persistence::StorageEntryKind entryKind(const std::string& path) override {
        return m_storage->entryKind(path);
    }
    void forEachEntry(const std::string& path,
                      const Persistence::StorageEntryVisitor& visitor) override {
        m_storage->forEachEntry(path, visitor);
    }
    void mkdirs(const std::string& path) override { m_storage->mkdirs(path); }
    bool createDirectoryExclusive(const std::string& path) override {
        return m_storage->createDirectoryExclusive(path);
    }
    bool createFileExclusive(const std::string& path,
                             const std::string& contents) override {
        return m_storage->createFileExclusive(path, contents);
    }
    std::unique_ptr<Persistence::WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& root) override {
        return m_storage->lockWorldGenerationBootstrap(root);
    }
    void remove(const std::string& path) override {
        if (path.ends_with("/publication-pending") && takeFault(Fault::RemovePending)) {
            throw std::runtime_error("injected pending-marker removal failure");
        }
        m_storage->remove(path);
    }
    void publishDirectory(const std::string& staged,
                          const std::string& final) override {
        m_storage->publishDirectory(staged, final);
    }

private:
    Fault m_fault;
    std::shared_ptr<Persistence::StorageBackend> m_storage;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_payloadWaiting = false;
    bool m_releasePayload = false;
};

CheckpointOutcome waitForCheckpoint(SimulationCheckpointManager& manager) {
    for (size_t attempt = 0; attempt < 100'000; ++attempt) {
        if (auto outcome = manager.poll()) return *outcome;
        std::this_thread::yield();
    }
    throw Test::TestFailure("checkpoint writer did not terminate");
}

std::vector<uint8_t> readStorageBytes(
    Persistence::StorageBackend& storage, const std::string& path
) {
    auto reader = storage.openRead(path);
    return reader->readAt(0, reader->size());
}

uint64_t readBigU64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | bytes.at(offset + i);
    return value;
}

uint64_t testHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void writeBigU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (24 - i * 8));
    }
}

void writeBigU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (56 - i * 8));
    }
}

struct CheckpointLayout {
    struct Receipt {
        size_t command = 0;
        size_t admission = 0;
        size_t pending = 0;
        size_t action = 0;
        size_t direction = 0;
        size_t outcome = 0;
        size_t mutationCount = 0;
    };

    std::array<size_t, 8> replayLimits{};
    size_t dictionaryCount = 0;
    std::vector<size_t> dictionaryValues;
    size_t nextEntity = 0;
    std::vector<Receipt> receipts;
    std::vector<size_t> entities;
    std::vector<size_t> entityTagCounts;
    size_t chunkCount = 0;
};

CheckpointLayout checkpointLayout(const std::vector<uint8_t>& bytes) {
    CheckpointLayout result;
    size_t at = 0;
    auto u8 = [&] { return bytes.at(at++); };
    auto u32 = [&] {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) value = (value << 8) | bytes.at(at + i);
        at += 4;
        return value;
    };
    auto skip = [&](size_t count) {
        if (count > bytes.size() - at) throw Test::TestFailure("invalid checkpoint fixture");
        at += count;
    };
    auto string = [&] { const size_t count = u32(); skip(count); };
    auto semantic = [&] { string(); skip(1); };
    auto entity = [&] {
        const size_t id = at;
        skip(16); string(); skip(48 + 4 + 4 + 4 + 24);
        result.entityTagCounts.push_back(at);
        const size_t tags = u32();
        for (size_t i = 0; i < tags; ++i) string();
        string(); skip(16);
        return id;
    };
    auto command = [&](CheckpointLayout::Receipt& result) {
        result.command = at;
        skip(8); skip(8); skip(16); skip(4); string(); skip(32);
        result.action = at; skip(1);
        if (u8()) {
            skip(12 + 12 + 4 + 12);
            result.direction = at; skip(1); semantic();
        }
        result.mutationCount = at;
        const size_t mutations = u32();
        for (size_t i = 0; i < mutations; ++i) {
            skip(12); semantic(); semantic();
        }
    };

    skip(8 + 4 + 8 + 8 + 32 + 4); string();
    skip(24 + 8 + 4);
    for (size_t i = 0; i < result.replayLimits.size(); ++i) {
        result.replayLimits[i] = at;
        skip(8);
    }
    result.dictionaryCount = at;
    const size_t dictionary = u32();
    for (size_t i = 0; i < dictionary; ++i) {
        const size_t size = u32();
        result.dictionaryValues.push_back(at);
        skip(size);
    }
    skip(8 + 8 + 8);
    result.nextEntity = at; skip(4);
    skip(8 + 4 + 8 + 8 + 16);
    const size_t receipts = u32();
    result.receipts.resize(receipts);
    for (auto& receipt : result.receipts) {
        command(receipt);
        receipt.admission = at; skip(8);
        receipt.pending = at;
        const bool pending = u8();
        const bool outcome = u8();
        if (pending == outcome) throw Test::TestFailure("invalid checkpoint fixture phase");
        if (outcome) {
            receipt.outcome = at;
            skip(8 * 5 + 1);
        }
    }
    const size_t entities = u32();
    for (size_t i = 0; i < entities; ++i) result.entities.push_back(entity());
    result.chunkCount = at;
    return result;
}

CheckpointRecoveryStatus recoverMutatedCheckpoint(
    HostFixture& fixture,
    const std::function<void(std::vector<uint8_t>&, const CheckpointLayout&)>& mutate,
    const std::function<void()>& beforeRecover = {},
    const std::function<void()>& afterRecover = {}
) {
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    {
        SimulationCheckpointManager manager(storage, "/save");
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    }
    auto payload = readStorageBytes(*storage, "/save/checkpoints/1.bin");
    const auto layout = checkpointLayout(payload);
    mutate(payload, layout);
    {
        auto write = storage->openWrite("/save/checkpoints/1.bin");
        write->writer().writeBytes(payload.data(), payload.size());
        write->commit();
    }
    auto pointer = readStorageBytes(*storage, "/save/current");
    writeBigU64(pointer, 32, testHash(payload));
    writeBigU64(pointer, 40, payload.size());
    {
        auto write = storage->openWrite("/save/current");
        write->writer().writeBytes(pointer.data(), pointer.size());
        write->commit();
    }
    SimulationCheckpointManager reopened(storage, "/save");
    SimulationHostConfig currentPolicy;
    currentPolicy.maxSnapshotCells = 25'000;
    if (beforeRecover) beforeRecover();
    const auto status = reopened.recover(
        fixture.resources, fixture.generator, currentPolicy).status;
    if (afterRecover) afterRecover();
    return status;
}

void pumpBaseline(LoopbackReplica& replica) {
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Idle);
}

} // namespace

TEST_CASE(SimulationHost_exact_reads_preserve_known_air_and_unavailable) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {63, 7, 7}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 8'000;
    HostFixture fixture(config);

    CHECK_EQ(fixture.host->read({1, 7, 1}).status, ExactReadStatus::Known);
    CHECK_EQ(
        fixture.host->read({1, 7, 1}).state.blockKey,
        std::string("base:air"));
    CHECK_EQ(fixture.host->read({40, 7, 1}).status, ExactReadStatus::Unavailable);
    CHECK_EQ(fixture.host->read({64, 7, 1}).status, ExactReadStatus::OutsideDomain);
}

TEST_CASE(SimulationHost_exact_reads_report_invalid_stored_state) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {31, 7, 7}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 8'000;
    HostFixture fixture(config);

    const CellAddress address{1, 7, 1};
    auto& world = const_cast<Voxel::World&>(fixture.host->world());
    auto* chunk = world.chunkManager().getChunk({0, 0, 0});
    CHECK(chunk != nullptr);
    chunk->setBlock(
        address.x, address.y, address.z,
        {Voxel::BlockID{std::numeric_limits<uint16_t>::max()}, 0, 0});

    CHECK_EQ(fixture.host->read(address).status, ExactReadStatus::InvalidState);
}

TEST_CASE(SimulationHost_entity_motion_stops_at_unavailable_frontier) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {63, 7, 7}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 8'000;
    HostFixture fixture(config, {2.0f, 4.0f, 2.0f});

    auto moving = std::make_unique<Entity::Entity>();
    moving->setPosition({30.0f, 4.0f, 2.0f});
    moving->setVelocity({600.0f, 0.0f, 0.0f});
    const auto movingId = fixture.host->spawnEntity(std::move(moving));
    CHECK(!movingId.isNull());
    fixture.host->advance(17ms);
    const auto* entity = fixture.host->world().entities().get(movingId);
    CHECK(entity != nullptr);
    CHECK(entity->position().x <= 31.5f);
}

TEST_CASE(SimulationHost_atomic_failure_changes_neither_chunk) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {63, 7, 7}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 8'000;
    HostFixture fixture(config);
    fixture.start();

    const CellAddress available{1, 7, 1};
    const CellAddress unavailable{40, 7, 1};
    EditCommand command{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
        .mutations = {
            {available, {"base:air", 0}, {"rigel:stone", 0}},
            {unavailable, {"base:air", 0}, {"rigel:stone", 0}},
        },
    };
    const auto capability = fixture.host->authorityEditCapability();
    CHECK_EQ(
        fixture.host->submit(command, capability).status,
        SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->advance(17ms).ticksRun, static_cast<size_t>(1));
    CHECK_EQ(
        fixture.host->submit(command, capability).outcome->status,
        CommandOutcomeStatus::Unavailable);
    CHECK_EQ(
        fixture.host->read(available).state.blockKey,
        std::string("base:air"));
    CHECK_EQ(fixture.host->read(unavailable).status, ExactReadStatus::Unavailable);
}

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
TEST_CASE(SimulationHost_prepared_atomic_writes_survive_allocation_failure) {
    bool observedFailure = false;
    bool observedSuccess = false;
    for (size_t failureIndex = 0; failureIndex < 16; ++failureIndex) {
        SimulationHostConfig config;
        config.domain = {{0, 0, 0}, {31, 31, 30}};
        config.preloadedChunks = {{0, 0, 0}};
        config.maxPreloadedChunks = 1;
        config.maxSnapshotCells = 32'768;
        HostFixture fixture(config);
        fixture.start();
        const auto capability = fixture.host->authorityEditCapability();
        const CellAddress first{10, 20, 10};
        const CellAddress second{11, 20, 10};

        EditCommand seed{
            .session = 1,
            .command = 1,
            .actor = fixture.actor,
            .world = 0,
            .zone = "base:default",
            .content = fixture.host->content().identity(),
            .action = EditAction::Atomic,
            .mutations = {{first, {"base:air", 0},
                           {"rigel:stone", 0}}},
        };
        CHECK_EQ(
            fixture.host->submit(seed, capability).status,
            SubmitStatus::Accepted);
        fixture.host->advance(17ms);
        CHECK_EQ(fixture.host->read(first).state.blockKey,
                 std::string("rigel:stone"));

        EditCommand move{
            .session = 1,
            .command = 2,
            .actor = fixture.actor,
            .world = 0,
            .zone = "base:default",
            .content = fixture.host->content().identity(),
            .action = EditAction::Atomic,
            .mutations = {
                {first, {"rigel:stone", 0}, {"base:air", 0}},
                {second, {"base:air", 0}, {"rigel:stone", 0}},
            },
        };
        CHECK_EQ(
            fixture.host->submit(move, capability).status,
            SubmitStatus::Accepted);

        allocationsBeforeFailure = failureIndex;
        failureAllocationSize = 0;
        failAllocation = true;
        bool threw = false;
        try {
            fixture.host->advance(17ms);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        failAllocation = false;

        if (threw) {
            observedFailure = true;
            CHECK_EQ(fixture.host->tick(), Tick{1});
            CHECK_EQ(fixture.host->revision(), Revision{1});
            CHECK_EQ(fixture.host->read(first).state.blockKey,
                     std::string("rigel:stone"));
            CHECK_EQ(fixture.host->read(second).state.blockKey,
                     std::string("base:air"));
            CHECK_EQ(
                fixture.host->submit(move, capability).status,
                SubmitStatus::DuplicatePending);
        } else {
            observedSuccess = true;
            CHECK_EQ(fixture.host->read(first).state.blockKey,
                     std::string("base:air"));
            CHECK_EQ(fixture.host->read(second).state.blockKey,
                     std::string("rigel:stone"));
        }
    }
    CHECK(observedFailure);
    CHECK(observedSuccess);
}

TEST_CASE(SimulationHost_second_empty_subchunk_prepare_is_atomic_on_failure) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {31, 31, 30}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 32'768;
    HostFixture fixture(config);
    fixture.start();
    const auto capability = fixture.host->authorityEditCapability();
    const CellAddress first{10, 20, 10};
    const CellAddress second{20, 20, 10};
    EditCommand command{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
        .mutations = {
            {first, {"base:air", 0}, {"rigel:stone", 0}},
            {second, {"base:air", 0}, {"rigel:stone", 0}},
        },
    };
    CHECK_EQ(
        fixture.host->submit(command, capability).status,
        SubmitStatus::Accepted);

    failureAllocationSize = sizeof(std::array<
        Voxel::BlockState, Voxel::Chunk::SUBCHUNK_VOLUME>);
    allocationsBeforeFailure = 1;
    failAllocation = true;
    CHECK_THROWS(fixture.host->advance(17ms));
    const bool reachedSecondSubchunkAllocation = !failAllocation;
    failAllocation = false;
    failureAllocationSize = 0;
    CHECK(reachedSecondSubchunkAllocation);

    CHECK_EQ(fixture.host->tick(), Tick{0});
    CHECK_EQ(fixture.host->revision(), Revision{0});
    CHECK_EQ(fixture.host->read(first).state.blockKey, std::string("base:air"));
    CHECK_EQ(fixture.host->read(second).state.blockKey, std::string("base:air"));
    CHECK_EQ(
        fixture.host->submit(command, capability).status,
        SubmitStatus::DuplicatePending);

    CHECK_EQ(fixture.host->advance(0ns).ticksRun, static_cast<size_t>(1));
    CHECK_EQ(fixture.host->read(first).state.blockKey, std::string("rigel:stone"));
    CHECK_EQ(fixture.host->read(second).state.blockKey, std::string("rigel:stone"));
    const auto completed = fixture.host->submit(command, capability);
    CHECK_EQ(completed.status, SubmitStatus::DuplicateComplete);
    CHECK_EQ(completed.outcome->status, CommandOutcomeStatus::Applied);
}
#endif

TEST_CASE(SimulationHost_recording_resimulates_same_cut_across_frame_pacing) {
    SimulationHostConfig config;
    config.domain = {{-4, -4, -4}, {20, 8, 20}};
    config.maxPreloadedChunks = 8;
    config.maxSnapshotCells = 25'000;
    config.maxCatchUpTicks = 1;
    const std::vector<std::vector<std::chrono::nanoseconds>> pacingPatterns{
        {33'333'333ns}, {16'666'667ns}, {6'944'444ns}, {250ms, 1ms, 7ms}};

    std::vector<uint64_t> expectedHashes;
    std::optional<SimulationRecording> expectedRecording;
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        for (const auto& pacing : pacingPatterns) {
            HostFixture fixture(
                config, {5.5f, 6.0f, 5.5f}, {1.25f, 0.0f, -0.5f});
            fixture.start();
            CHECK_EQ(fixture.host->submit(fixture.removeCommand(1)).status,
                     SubmitStatus::Accepted);
            std::vector<uint64_t> hashes;
            std::optional<Tick> admissionsAppliedAt;
            Entity::EntityId temporary;
            size_t frame = 0;
            bool drainDebt = false;
            while (fixture.host->tick() < 20) {
                const Tick before = fixture.host->tick();
                if (admissionsAppliedAt != before) {
                    admissionsAppliedAt = before;
                    if (before == 1) {
                        auto rejected = fixture.removeCommand(90);
                        rejected.action = static_cast<EditAction>(255);
                        CHECK_EQ(fixture.host->submit(rejected).status,
                                 SubmitStatus::InvalidRequest);
                    } else if (before == 2) {
                        CHECK_EQ(fixture.host->submit(fixture.removeCommand(2)).status,
                                 SubmitStatus::Accepted);
                    } else if (before == 3) {
                        auto entity = std::make_unique<Entity::Entity>();
                        entity->addTag(Entity::EntityTags::NoClip);
                        entity->setPosition({8.0f, 7.0f, 8.0f});
                        temporary = fixture.host->spawnEntity(std::move(entity));
                        CHECK(!temporary.isNull());
                    } else if (before == 4) {
                        CHECK(fixture.host->despawnEntity(temporary));
                    } else if (before == 5) {
                        CHECK_EQ(fixture.host->startSession(
                                     2, fixture.actor,
                                     fixture.host->content().identity()),
                                 SessionStartStatus::Started);
                        CHECK_EQ(fixture.host->submit(fixture.removeCommand(91)).status,
                                 SubmitStatus::OldSession);
                    } else if (before == 6) {
                        auto command = fixture.removeCommand(3);
                        command.session = 2;
                        CHECK_EQ(fixture.host->submit(command).status,
                                 SubmitStatus::Accepted);
                    }
                }
                const auto elapsed = drainDebt
                    ? 0ns : pacing[frame++ % pacing.size()];
                const auto advanced = fixture.host->advance(elapsed);
                if (advanced.ticksRun == 1) hashes.push_back(fixture.host->stateHash());
                drainDebt = advanced.timeDebtRemaining;
            }
            const auto recording = fixture.host->recording();
            CHECK(recording.has_value());
            CHECK_EQ(recording->finalHash, hashes.back());
            if (expectedHashes.empty()) {
                expectedHashes = hashes;
                expectedRecording = recording;
            } else {
                CHECK_EQ(hashes, expectedHashes);
                CHECK_EQ(recording->bytes, expectedRecording->bytes);
            }
        }
    }

    HostFixture replayFixture(config);
    for (const auto& pacing : pacingPatterns) {
        auto replayed = SimulationHost::resimulate(
            replayFixture.resources, replayFixture.generator,
            *expectedRecording, pacing);
        CHECK_EQ(replayed.status, ResimulationStatus::Complete);
        CHECK_EQ(replayed.tick, expectedRecording->finalTick);
        CHECK_EQ(replayed.stateHash, expectedRecording->finalHash);
        CHECK(replayed.host != nullptr);
    }
}

TEST_CASE(SimulationHost_recording_rejects_envelope_mismatch_and_event_gap) {
    SimulationHostConfig config;
    config.domain = {{-4, -4, -4}, {20, 8, 20}};
    config.maxPreloadedChunks = 8;
    config.maxSnapshotCells = 25'000;
    HostFixture fixture(config);
    fixture.start();
    fixture.host->advance(17ms);
    const auto recording = fixture.host->recording();
    CHECK(recording.has_value());

    auto incompatibleGenerator = std::make_shared<Voxel::WorldGenerator>(
        fixture.resources.registry(), flatDefinition(), 18);
    auto mismatch = SimulationHost::resimulate(
        fixture.resources, incompatibleGenerator, *recording, {17ms});
    CHECK_EQ(mismatch.status, ResimulationStatus::EnvelopeMismatch);

    SimulationHostConfig bounded = config;
    bounded.maxReplayEvents = 1;
    HostFixture overflow(bounded);
    overflow.start();
    CHECK(!overflow.host->recording().has_value());

    bounded.maxReplayEvents = 64;
    bounded.maxReplayBytes = 3 * 1024 * 1024;
    bounded.zone = std::string(60 * 1024, 'z');
    HostFixture byteOverflow(bounded);
    byteOverflow.start();
    CHECK(byteOverflow.host->recording().has_value());
    for (CommandId id = 1; id <= 64; ++id) {
        auto retained = byteOverflow.removeCommand(id);
        retained.zone = bounded.zone;
        CHECK_EQ(byteOverflow.host->submit(std::move(retained)).status,
                 SubmitStatus::Accepted);
    }
    CHECK(!byteOverflow.host->recording().has_value());
    CHECK_EQ(byteOverflow.host->advance(17ms).ticksRun, size_t{1});
    CHECK(!byteOverflow.host->recording().has_value());
}

TEST_CASE(SimulationRecording_replays_the_recorded_admission_envelope) {
    SimulationHostConfig config;
    config.maxChangesPerCommand = 9;
    HostFixture fixture(config);
    fixture.start();

    EditCommand command{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
    };
    for (int x = 2; x < 11; ++x) {
        const CellAddress address{x, 7, 3};
        const auto current = fixture.host->read(address);
        CHECK_EQ(current.status, ExactReadStatus::Known);
        CHECK_EQ(current.state.blockKey, std::string("base:air"));
        command.mutations.push_back({
            address, current.state, {"rigel:stone", 0}});
    }
    CHECK_EQ(
        fixture.host->submit(
            command, fixture.host->authorityEditCapability()).status,
        SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    const auto recording = fixture.host->recording();
    CHECK(recording.has_value());

    auto replayed = SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording,
        {7ms, 19ms, 3ms});
    CHECK_EQ(replayed.status, ResimulationStatus::Complete);
    CHECK_EQ(replayed.stateHash, recording->finalHash);
}

TEST_CASE(SimulationHost_rejects_nonfinite_entity_tint_before_admission) {
    HostFixture fixture;
    const size_t before = fixture.host->world().entities().size();
    for (const float invalid : {
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity()}) {
        for (int component = 0; component < 4; ++component) {
            auto entity = std::make_unique<Entity::Entity>();
            glm::vec4 tint(1.0f);
            tint[component] = invalid;
            entity->setRenderTint(tint);
            CHECK(fixture.host->spawnEntity(std::move(entity)).isNull());
            CHECK_EQ(fixture.host->world().entities().size(), before);
        }
    }
}

TEST_CASE(SimulationHost_checkpoint_capture_rejects_nonfinite_live_state) {
    HostFixture fixture;
    auto& world = const_cast<Voxel::World&>(fixture.host->world());
    world.entities().get(fixture.actor)->setVelocity(
        {std::numeric_limits<float>::infinity(), 0, 0});
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/invalid-state");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::UnsupportedState);
    CHECK(!storage->exists("/invalid-state/current"));
    CHECK(!fixture.host->recording().has_value());
}

TEST_CASE(SimulationHost_recording_decoder_rejects_nonfinite_entity_scalar) {
    HostFixture fixture({}, {5.5f, 6.0f, 5.5f}, {123.25f, 0, 0});
    auto recording = fixture.host->recording();
    CHECK(recording.has_value());
    const uint32_t bits = std::bit_cast<uint32_t>(123.25f);
    const std::array<uint8_t, 4> marker{
        static_cast<uint8_t>(bits >> 24), static_cast<uint8_t>(bits >> 16),
        static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)};
    auto& bytes = recording->bytes;
    const auto scalar = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    CHECK(scalar != bytes.end());
    CHECK(std::search(scalar + 4, bytes.end(), marker.begin(), marker.end()) == bytes.end());
    // The current experimental recording uses big-endian IEEE float scalars.
    const std::array<uint8_t, 4> infinity{0x7f, 0x80, 0, 0};
    std::copy(infinity.begin(), infinity.end(), scalar);
    CHECK_EQ(SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms}).status,
        ResimulationStatus::MalformedRecording);
}

TEST_CASE(SimulationHost_bounds_live_and_recorded_snapshot_execution) {
    SimulationHostConfig bounded;
    bounded.domain = {{0, 0, 0}, {7, 7, 7}};
    bounded.maxSnapshotCells = 1'048'576;
    HostFixture atLimit(bounded);
    ++bounded.maxSnapshotCells;
    CHECK_THROWS(HostFixture{bounded});

    SimulationHostConfig policy;
    policy.maxInteractionDistance = 200'000'000.0f;
    HostFixture fixture(policy);
    fixture.start();
    auto command = fixture.removeCommand(1);
    command.interaction->direction = {0, 0, 1};
    command.interaction->maxDistance = policy.maxInteractionDistance;
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);

    // An excessive saved budget is corrupt before pending commands or terrain
    // are reconstructed, even when a finite, very long interaction is pending.
    CHECK_EQ(recoverMutatedCheckpoint(fixture, [](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.replayLimits.at(7),
                    std::numeric_limits<uint64_t>::max());
    }), CheckpointRecoveryStatus::Corrupt);

    fixture.host->advance(17ms);
    const auto outcome = fixture.host->submit(command).outcome;
    CHECK(outcome.has_value());
    CHECK_EQ(outcome->status, CommandOutcomeStatus::InvalidRequest);
    const auto recording = fixture.host->recording();
    CHECK(recording.has_value());
    const auto valid = SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms});
    CHECK_EQ(valid.status, ResimulationStatus::Complete);
    CHECK_EQ(valid.stateHash, recording->finalHash);

    constexpr size_t baselineLengthOffset = 8 + 4 + 32 + 8 + 8;
    constexpr size_t baselineOffset = baselineLengthOffset + 8;
    const size_t baselineSize = readBigU64(recording->bytes, baselineLengthOffset);
    CHECK(baselineOffset + baselineSize <= recording->bytes.size());
    const std::vector<uint8_t> baseline(
        recording->bytes.begin() + baselineOffset,
        recording->bytes.begin() + baselineOffset + baselineSize);
    const auto layout = checkpointLayout(baseline);
    for (const uint64_t budget : {
             uint64_t{1'048'577}, std::numeric_limits<uint64_t>::max()}) {
        auto malformed = *recording;
        writeBigU64(malformed.bytes, baselineOffset + layout.replayLimits.at(7),
                    budget);
        const auto rejected = SimulationHost::resimulate(
            fixture.resources, fixture.generator, malformed, {17ms});
        CHECK_EQ(rejected.status, ResimulationStatus::MalformedRecording);
        CHECK(!rejected.host);
    }
}

TEST_CASE(SimulationHost_resimulation_derives_content_reconstruction_budget) {
    Voxel::WorldResources resources;
    // The registry permits an empty key; dictionary reconstruction must retain
    // the same key domain as the producer.
    resources.registry().registerBlock("", Voxel::BlockType{});
    for (const std::string identifier : {
             "rigel:stone", "rigel:grass", "rigel:water"}) {
        Voxel::BlockType type;
        type.identifier = identifier;
        resources.registry().registerBlock(identifier, std::move(type));
    }
    for (size_t index = 0; index < 260; ++index) {
        const std::string identifier =
            "rigel:large_" + std::to_string(index) + "_" +
            std::string(65'536, static_cast<char>('a' + index % 26));
        Voxel::BlockType type;
        type.identifier = identifier;
        resources.registry().registerBlock(identifier, std::move(type));
    }
    resources.registry().freeze();
    auto generator = std::make_shared<Voxel::WorldGenerator>(
        resources.registry(), flatDefinition(), 17);
    const auto requirement = ContentDictionary::retainedStorageRequirement(
        resources.registry());
    CHECK(requirement.has_value());
    CHECK(*requirement > ContentDictionary::kDefaultMaxRetainedBytes);
    CHECK(*requirement <= ContentDictionary::kMaximumRetainedBytes);

    SimulationHostConfig policy;
    policy.domain = {{0, 0, 0}, {0, 0, 0}};
    policy.preloadedChunks = {{0, 0, 0}};
    policy.maxPreloadedChunks = 1;
    policy.maxSnapshotCells = 1;
    policy.maxContentBytes = *requirement;
    SimulationHost host(resources, generator, policy);
    host.advance(17ms);
    const auto recording = host.recording();
    CHECK(recording.has_value());
    const auto replay = SimulationHost::resimulate(
        resources, generator, *recording, {3ms, 7ms, 11ms});
    CHECK_EQ(replay.status, ResimulationStatus::Complete);
    CHECK_EQ(replay.stateHash, host.stateHash());
    CHECK_EQ(replay.stateHash, recording->finalHash);
    CHECK(replay.host->content().retainedStorageBytes().has_value());
    CHECK(*replay.host->content().retainedStorageBytes() >
          ContentDictionary::kDefaultMaxRetainedBytes);
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/large-content");
    CHECK_EQ(manager.request(host), CheckpointRequestStatus::Started);
    std::optional<CheckpointOutcome> saved;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!saved && std::chrono::steady_clock::now() < deadline) {
        saved = manager.poll();
        if (!saved) std::this_thread::sleep_for(1ms);
    }
    CHECK(saved.has_value());
    CHECK_EQ(saved->status, CheckpointWriteStatus::Durable);
    auto recovered = manager.recover(resources, generator, policy);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.host->stateHash(), host.stateHash());
}

TEST_CASE(SimulationHost_preflights_forged_recording_dictionary_count) {
    HostFixture fixture;
    auto recording = fixture.host->recording();
    CHECK(recording.has_value());
    constexpr size_t baselineLengthOffset = 8 + 4 + 32 + 8 + 8;
    constexpr size_t baselineOffset = baselineLengthOffset + 8;
    const size_t baselineSize = readBigU64(recording->bytes, baselineLengthOffset);
    const std::vector<uint8_t> baseline(
        recording->bytes.begin() + baselineOffset,
        recording->bytes.begin() + baselineOffset + baselineSize);
    const auto layout = checkpointLayout(baseline);
    writeBigU32(
        recording->bytes, baselineOffset + layout.dictionaryCount, 65'536);

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
    // The fixture contains several chunks; malformed content must be rejected
    // without first duplicating its embedded checkpoint payload.
    CHECK(baselineSize > 1024 * 1024);
    watchedAllocationSize = baselineSize;
    watchedAllocationObserved = false;
#endif
    const auto replay = SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms});
#ifdef RIGEL_TEST_ALLOCATION_FAILURES
    watchedAllocationSize = 0;
    CHECK(!watchedAllocationObserved);
#endif
    CHECK_EQ(replay.status, ResimulationStatus::MalformedRecording);
    CHECK(!replay.host);
}

TEST_CASE(SimulationCheckpoint_rejects_mismatched_dictionary_before_content_copy) {
    HostFixture fixture(
        {}, {5.5f, 6.0f, 5.5f}, {0.0f, 0.0f, 0.0f}, 100);
    bool contentCopyAttempted = false;
    const auto status = recoverMutatedCheckpoint(
        fixture,
        [](auto& bytes, const auto& layout) {
            CHECK(!layout.dictionaryValues.empty());
            bytes.at(layout.dictionaryValues.front()) ^= 1;
        },
        [&] {
#ifdef RIGEL_TEST_ALLOCATION_FAILURES
            failureAllocationSize = fixture.resources.registry().size() *
                sizeof(ContentDictionaryEntry);
            allocationsBeforeFailure = 0;
            failAllocation = true;
#endif
        },
        [&] {
#ifdef RIGEL_TEST_ALLOCATION_FAILURES
            contentCopyAttempted = !failAllocation;
            failAllocation = false;
            failureAllocationSize = 0;
#endif
        });
    CHECK_EQ(status, CheckpointRecoveryStatus::Incompatible);
    CHECK(!contentCopyAttempted);
}

TEST_CASE(SimulationCheckpoint_incompatible_registry_preserves_compatibility_status) {
    HostFixture fixture;
    const auto recording = fixture.host->recording();
    CHECK(recording.has_value());
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/registry-compatibility");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    for (const bool changeCount : {false, true}) {
        Voxel::WorldResources other;
        for (size_t i = 1; i < fixture.resources.registry().size(); ++i) {
            auto type = fixture.resources.registry().getType(
                Voxel::BlockID{static_cast<uint16_t>(i)});
            if (!changeCount && type.identifier == "rigel:overhang") {
                type.identifier = "rigel:renamed_overhang";
            }
            const auto key = type.identifier;
            other.registry().registerBlock(key, std::move(type));
        }
        if (changeCount) {
            Voxel::BlockType extra;
            extra.identifier = "rigel:extra";
            other.registry().registerBlock(extra.identifier, extra);
        }
        other.registry().freeze();
        auto generator = std::make_shared<Voxel::WorldGenerator>(
            other.registry(), flatDefinition(), 17);
        CHECK_EQ(manager.recover(other, generator).status,
                 CheckpointRecoveryStatus::Incompatible);
        CHECK_EQ(SimulationHost::resimulate(
            other, generator, *recording, {17ms}).status,
            ResimulationStatus::EnvelopeMismatch);
    }
}

TEST_CASE(SimulationCheckpoint_rejects_noncanonical_dictionary) {
    HostFixture fixture;
    CHECK_EQ(recoverMutatedCheckpoint(fixture, [](auto& bytes, const auto& layout) {
        bytes.at(layout.dictionaryValues.front()) = 'z';
    }), CheckpointRecoveryStatus::Corrupt);
}

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
TEST_CASE(SimulationHost_failed_spawn_does_not_consume_authority_identity) {
    HostFixture fixture;
    auto failed = std::make_unique<Entity::Entity>();
    allocationsBeforeFailure = 0;
    failureAllocationSize = 0;
    failAllocation = true;
    CHECK_THROWS(fixture.host->spawnEntity(std::move(failed)));
    failAllocation = false;

    const auto admitted = fixture.host->spawnEntity(
        std::make_unique<Entity::Entity>());
    CHECK_EQ(admitted.counter, fixture.actor.counter + 1);
}
#endif

TEST_CASE(SimulationCheckpoint_recovery_waits_for_consumption_of_the_write_outcome) {
    HostFixture fixture;
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/checkpoint-terminal");
    for (uint64_t generation = 1; generation <= 2; ++generation) {
        fixture.host->advance(17ms);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (manager.writeInFlight() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        CHECK(!manager.writeInFlight());
        CHECK_EQ(manager.recover(fixture.resources, fixture.generator).status,
                 CheckpointRecoveryStatus::PublicationPending);
        const auto outcome = manager.poll();
        CHECK(outcome.has_value());
        CHECK_EQ(outcome->status, CheckpointWriteStatus::Durable);
        CHECK_EQ(outcome->generation, generation);
        auto recovered = manager.recover(fixture.resources, fixture.generator);
        CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
        CHECK_EQ(recovered.generation, generation);
        CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());
        fixture.host = std::move(recovered.host);
    }
}

TEST_CASE(SimulationCheckpoint_three_chunk_limit_round_trips) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {95, 3, 3}};
    config.maxPreloadedChunks = 3;
    HostFixture fixture(config);
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/checkpoint-three-chunks");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    auto recovered = manager.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());
    CHECK_EQ(recovered.host->read({94, 0, 1}).status, ExactReadStatus::Known);
    CHECK_EQ(recovered.host->read({94, 0, 1}).state, fixture.host->read({94, 0, 1}).state);
}

TEST_CASE(SimulationHost_recording_rejects_cuts_and_admissions_before_the_baseline) {
    HostFixture fixture;
    fixture.host->advance(50ms);
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/checkpoint-recording-cut");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    auto recovered = manager.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    auto recording = recovered.host->recording();
    CHECK(recording.has_value());
    const Tick baselineTick = recording->finalTick;
    CHECK(baselineTick > Tick{0});
    const auto valid = SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms});
    CHECK_EQ(valid.status, ResimulationStatus::Complete);
    CHECK_EQ(valid.tick, baselineTick);

    --recording->finalTick;
    writeBigU64(recording->bytes, 8 + 4 + 32, recording->finalTick);
    CHECK_EQ(SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms}).status,
        ResimulationStatus::MalformedRecording);

    CHECK_EQ(recovered.host->startSession(
        1, fixture.actor, recovered.host->content().identity()), SessionStartStatus::Started);
    recording = recovered.host->recording();
    CHECK(recording.has_value());
    CHECK_EQ(SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms}).status,
        ResimulationStatus::Complete);
    // Current recording header is followed by its length-prefixed baseline,
    // admission count, and the first admission's afterTick.
    constexpr size_t baselineLengthOffset = 8 + 4 + 32 + 8 + 8;
    const size_t firstAdmission = baselineLengthOffset + 8 +
        static_cast<size_t>(readBigU64(recording->bytes, baselineLengthOffset)) + 4;
    writeBigU64(recording->bytes, firstAdmission, baselineTick - 1);
    CHECK_EQ(SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms}).status,
        ResimulationStatus::MalformedRecording);
}

TEST_CASE(SimulationCheckpoint_rebinds_compact_ids_to_saved_semantic_keys) {
    HostFixture fixture;
    fixture.start();
    const auto command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    Voxel::WorldResources reordered;
    const auto& original = fixture.resources.registry();
    for (size_t index = original.size(); index-- > 1;) {
        const auto& type = original.getType(Voxel::BlockID{static_cast<uint16_t>(index)});
        reordered.registry().registerBlock(type.identifier, type);
    }
    reordered.registry().freeze();
    auto generator = std::make_shared<Voxel::WorldGenerator>(
        reordered.registry(), flatDefinition(), 17);
    CHECK(original.findByIdentifier("rigel:grass") !=
          reordered.registry().findByIdentifier("rigel:grass"));
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/checkpoint-reordered-content");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    auto recovered = manager.recover(reordered, generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());
    const CellAddress edited = command.mutations.front().address;
    CHECK_EQ(recovered.host->read(edited).state.blockKey, std::string("base:air"));
    CHECK_EQ(recovered.host->read({4, 0, 4}).state, fixture.host->read({4, 0, 4}).state);
    fixture.host->advance(17ms);
    recovered.host->advance(17ms);
    CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());
}

TEST_CASE(SimulationCheckpoint_restores_rules_under_current_runtime_policy) {
    SimulationHostConfig savedPolicy;
    savedPolicy.maxCatchUpTicks = 1;
    savedPolicy.maxInteractionDistance = 8.0f;
    HostFixture fixture(savedPolicy);
    fixture.start();
    const uint64_t expectedHash = fixture.host->stateHash();

    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/current-policy");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);

    SimulationHostConfig currentPolicy;
    currentPolicy.world = 77;
    currentPolicy.zone = "ignored:runtime-domain";
    currentPolicy.tickRate = {1, 1};
    currentPolicy.maxCatchUpTicks = 3;
    currentPolicy.maxSessionReceipts = 512;
    currentPolicy.maxReplicaQueue = 1;
    currentPolicy.maxInteractionDistance = 0.5f;
    auto recovered = manager.recover(
        fixture.resources, fixture.generator, currentPolicy);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.host->stateHash(), expectedHash);

    const EditCommand command = fixture.removeCommand(1);
    CHECK_EQ(recovered.host->submit(command).status, SubmitStatus::Accepted);
    CHECK_EQ(recovered.host->advance(100ms).ticksRun, size_t{3});
    CHECK_EQ(
        recovered.host->submit(command).outcome->status,
        CommandOutcomeStatus::Applied);
}

TEST_CASE(SimulationCheckpoint_refuses_insufficient_current_limits_without_loss) {
    SimulationHostConfig savedPolicy;
    savedPolicy.maxPendingCommands = 2;
    savedPolicy.maxSessionReceipts = 4;
    savedPolicy.maxSnapshotCells = 25'000;
    HostFixture fixture(savedPolicy);
    fixture.start();
    CHECK_EQ(fixture.host->submit(fixture.removeCommand(1)).status,
             SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->submit(fixture.removeCommand(2)).status,
             SubmitStatus::Accepted);
    auto extra = std::make_unique<Entity::Entity>();
    extra->addTag(Entity::EntityTags::NoClip);
    extra->setPosition({9.0f, 7.0f, 9.0f});
    CHECK(!fixture.host->spawnEntity(std::move(extra)).isNull());

    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/bounded-recovery");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    const auto pointer = readStorageBytes(*storage, "/bounded-recovery/current");
    const auto payload = readStorageBytes(
        *storage, "/bounded-recovery/checkpoints/1.bin");

    SimulationHostConfig insufficient;
    insufficient.maxPendingCommands = 1;
    insufficient.maxSessionReceipts = 1;
    insufficient.maxEntities = 1;
    CHECK_EQ(manager.recover(
        fixture.resources, fixture.generator, insufficient).status,
        CheckpointRecoveryStatus::Incompatible);
    auto insufficientContent = savedPolicy;
    insufficientContent.maxContentBytes = 1;
    CHECK_EQ(manager.recover(
        fixture.resources, fixture.generator, insufficientContent).status,
        CheckpointRecoveryStatus::Incompatible);
    CHECK_EQ(readStorageBytes(*storage, "/bounded-recovery/current"), pointer);
    CHECK_EQ(readStorageBytes(
        *storage, "/bounded-recovery/checkpoints/1.bin"), payload);

    auto recovered = manager.recover(
        fixture.resources, fixture.generator, savedPolicy);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.host->activeSession()->pendingCommands.size(), size_t{2});
    CHECK_EQ(recovered.host->world().entities().size(), size_t{2});
}

TEST_CASE(SimulationCheckpoint_preserves_interaction_execution_budget) {
    SimulationHostConfig savedPolicy;
    savedPolicy.domain = {{0, -4, 0}, {7, 3, 7}};
    savedPolicy.maxPreloadedChunks = 2;
    savedPolicy.maxSnapshotCells = 2'048;
    savedPolicy.maxInteractionDistance = 64.0f;
    HostFixture fixture(savedPolicy, {5.5f, 2.0f, 5.5f});
    fixture.start();
    auto command = fixture.removeCommand(1);
    command.interaction->maxDistance = 64.0f;
    const CellAddress target = command.mutations.front().address;
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);

    auto pendingStorage =
        std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager pendingManager(
        pendingStorage, "/pending-interaction-budget");
    CHECK_EQ(pendingManager.request(*fixture.host),
             CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(pendingManager).status,
             CheckpointWriteStatus::Durable);
    const auto pendingPointer = readStorageBytes(
        *pendingStorage, "/pending-interaction-budget/current");
    const auto pendingPayload = readStorageBytes(
        *pendingStorage,
        "/pending-interaction-budget/checkpoints/1.bin");

    for (const size_t changedBudget : {size_t{512}, size_t{4'096}}) {
        auto changedPolicy = savedPolicy;
        changedPolicy.maxSnapshotCells = changedBudget;
        auto rejected = pendingManager.recover(
            fixture.resources, fixture.generator, changedPolicy);
        CHECK_EQ(rejected.status, CheckpointRecoveryStatus::Incompatible);
        CHECK(rejected.detail.find("pending interaction commands") !=
              std::string::npos);
        CHECK(rejected.detail.find(std::to_string(savedPolicy.maxSnapshotCells)) !=
              std::string::npos);
        CHECK(rejected.detail.find(std::to_string(changedBudget)) !=
              std::string::npos);
        CHECK_EQ(readStorageBytes(
                     *pendingStorage,
                     "/pending-interaction-budget/current"),
                 pendingPointer);
        CHECK_EQ(readStorageBytes(
                     *pendingStorage,
                     "/pending-interaction-budget/checkpoints/1.bin"),
                 pendingPayload);
    }

    auto recovered = pendingManager.recover(
        fixture.resources, fixture.generator, savedPolicy);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    fixture.host->advance(17ms);
    recovered.host->advance(17ms);
    CHECK_EQ(fixture.host->read(target).state.blockKey,
             std::string("base:air"));
    CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());

    const auto recording = fixture.host->recording();
    CHECK(recording.has_value());
    auto replayed = SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {1ms, 16ms});
    CHECK_EQ(replayed.status, ResimulationStatus::Complete);
    CHECK_EQ(replayed.stateHash, fixture.host->stateHash());
    CHECK_EQ(replayed.host->read(target).state.blockKey,
             std::string("base:air"));

    auto drainedStorage =
        std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager drainedManager(
        drainedStorage, "/drained-interaction-budget");
    CHECK_EQ(drainedManager.request(*fixture.host),
             CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(drainedManager).status,
             CheckpointWriteStatus::Durable);
    auto nextCommand = fixture.removeCommand(2);
    nextCommand.interaction->maxDistance = 64.0f;
    const CellAddress nextTarget = nextCommand.mutations.front().address;

    auto lowerPolicy = savedPolicy;
    lowerPolicy.maxSnapshotCells = 512;
    auto lower = drainedManager.recover(
        fixture.resources, fixture.generator, lowerPolicy);
    CHECK_EQ(lower.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(lower.host->submit(nextCommand).status, SubmitStatus::Accepted);
    lower.host->advance(17ms);
    CHECK_EQ(lower.host->submit(nextCommand).outcome->status,
             CommandOutcomeStatus::InvalidRequest);
    CHECK_NE(lower.host->read(nextTarget).state.blockKey,
             std::string("base:air"));

    auto higherPolicy = savedPolicy;
    higherPolicy.maxSnapshotCells = 4'096;
    auto higher = drainedManager.recover(
        fixture.resources, fixture.generator, higherPolicy);
    CHECK_EQ(higher.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(higher.host->submit(nextCommand).status, SubmitStatus::Accepted);
    higher.host->advance(17ms);
    CHECK_EQ(higher.host->submit(nextCommand).outcome->status,
             CommandOutcomeStatus::Applied);
    CHECK_EQ(higher.host->read(nextTarget).state.blockKey,
             std::string("base:air"));
}

TEST_CASE(SimulationCheckpoint_payload_io_failures_preserve_the_acknowledged_cut) {
    for (const auto fault : {CheckpointFaultStorage::Fault::PayloadOpen,
             CheckpointFaultStorage::Fault::PayloadWrite,
             CheckpointFaultStorage::Fault::PayloadFlush,
             CheckpointFaultStorage::Fault::PayloadCommit,
             CheckpointFaultStorage::Fault::PayloadUncertain}) {
        SimulationHostConfig config;
        config.domain = {{0, 0, 0}, {7, 7, 7}};
        config.maxPreloadedChunks = 1;
        HostFixture fixture(config);
        Test::TemporaryDirectory temporary("rigel_checkpoint_payload_failure");
        const std::string root = (temporary.path() / "world").string();
        auto filesystem = std::make_shared<Persistence::FilesystemBackend>();
        auto faulted = std::make_shared<CheckpointFaultStorage>(
            CheckpointFaultStorage::Fault::None, filesystem);
        SimulationCheckpointManager manager(faulted, root);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
        const auto current = readStorageBytes(*filesystem, root + "/current");
        const auto payload = readStorageBytes(*filesystem, root + "/checkpoints/1.bin");
        fixture.host->advance(17ms);
        for (int retry = 0; retry < 2; ++retry) {
            faulted->setFault(fault);
            CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
            const auto failed = waitForCheckpoint(manager);
            CHECK_EQ(failed.status, CheckpointWriteStatus::NotPublished);
            CHECK_EQ(failed.generation, uint64_t{2});
            CHECK(!manager.durabilityUncertain());
            CHECK_EQ(readStorageBytes(*filesystem, root + "/current"), current);
            CHECK_EQ(readStorageBytes(*filesystem, root + "/checkpoints/1.bin"), payload);
            CHECK(!filesystem->exists(root + "/publication-pending"));
            size_t files = 0;
            filesystem->forEachEntry(root + "/checkpoints", [&](const std::string& name) {
                const auto leaf = std::filesystem::path(name).filename().string();
                CHECK(leaf == "0.bin" || leaf == "1.bin");
                ++files;
                return true;
            });
            CHECK(files <= size_t{2});
        }
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        const auto published = waitForCheckpoint(manager);
        CHECK_EQ(published.status, CheckpointWriteStatus::Durable);
        CHECK_EQ(published.generation, uint64_t{2});
        auto recovered = manager.recover(fixture.resources, fixture.generator);
        CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
        CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());
    }
}

TEST_CASE(SimulationCheckpoint_publishes_the_captured_cut_and_restores_pending_work) {
    HostFixture fixture;
    fixture.start();
    const EditCommand command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    const uint64_t capturedHash = fixture.host->stateHash();

    auto storage = std::make_shared<CheckpointFaultStorage>(
        CheckpointFaultStorage::Fault::DelayPayload);
    SimulationCheckpointManager manager(storage, "/save");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    storage->waitForPayload();

    fixture.host->advance(17ms);
    const uint64_t appliedHash = fixture.host->stateHash();
    CHECK(appliedHash != capturedHash);
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Coalesced);
    storage->releasePayload();
    const auto outcome = waitForCheckpoint(manager);
    CHECK_EQ(outcome.status, CheckpointWriteStatus::Durable);
    CHECK_EQ(outcome.stateHash, capturedHash);

    SimulationHostConfig currentPolicy;
    currentPolicy.maxSnapshotCells = 25'000;
    auto recovered = manager.recover(
        fixture.resources, fixture.generator, currentPolicy);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.generation, uint64_t{1});
    CHECK_EQ(recovered.host->stateHash(), capturedHash);
    CHECK_EQ(
        recovered.host->submit(command).status,
        SubmitStatus::DuplicatePending);
    recovered.host->advance(17ms);
    CHECK_EQ(recovered.host->stateHash(), appliedHash);

    CHECK_EQ(manager.request(*fixture.host),
             CheckpointRequestStatus::UnsupportedState);
    CHECK_EQ(manager.request(*recovered.host), CheckpointRequestStatus::Started);
    const auto newer = waitForCheckpoint(manager);
    CHECK_EQ(newer.status, CheckpointWriteStatus::Durable);
    CHECK_EQ(newer.generation, uint64_t{2});
    auto newest = manager.recover(fixture.resources, fixture.generator);
    CHECK_EQ(newest.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(newest.generation, uint64_t{2});
    CHECK_EQ(newest.host->stateHash(), appliedHash);
    auto next = std::make_unique<Entity::Entity>();
    const auto nextId = newest.host->spawnEntity(std::move(next));
    CHECK_EQ(nextId.counter, fixture.actor.counter + 1);
}

TEST_CASE(SimulationCheckpoint_preserves_unknown_roots_and_blocks_uncertainty) {
    HostFixture fixture;
    auto unknownStorage = std::make_shared<Persistence::InMemoryStorageBackend>();
    {
        auto write = unknownStorage->openWrite("/unknown/foreign.save");
        write->writer().writeU32(0x12345678);
        write->commit();
    }
    SimulationCheckpointManager unknown(unknownStorage, "/unknown");
    CHECK_EQ(
        unknown.request(*fixture.host),
        CheckpointRequestStatus::UnsupportedState);
    CHECK_EQ(
        unknown.recover(fixture.resources, fixture.generator).status,
        CheckpointRecoveryStatus::Incompatible);
    CHECK(unknownStorage->exists("/unknown/foreign.save"));

    auto partialStorage = std::make_shared<Persistence::InMemoryStorageBackend>();
    {
        SimulationCheckpointManager initialized(partialStorage, "/partial");
    }
    // Reach missing-payload-directory validation within a recognized format.
    partialStorage->remove("/partial/checkpoints");
    {
        auto write = partialStorage->openWrite("/partial/current");
        write->writer().writeU32(0x12345678);
        write->commit();
    }
    SimulationCheckpointManager partial(partialStorage, "/partial");
    CHECK_EQ(
        partial.recover(fixture.resources, fixture.generator).status,
        CheckpointRecoveryStatus::Corrupt);
    CHECK(!partialStorage->exists("/partial/checkpoints"));
    CHECK(partialStorage->exists("/partial/current"));

    auto uncertainStorage = std::make_shared<CheckpointFaultStorage>(
        CheckpointFaultStorage::Fault::PointerUncertain);
    SimulationCheckpointManager uncertain(uncertainStorage, "/save");
    CHECK_EQ(uncertain.request(*fixture.host), CheckpointRequestStatus::Started);
    const auto failed = waitForCheckpoint(uncertain);
    CHECK_EQ(failed.status, CheckpointWriteStatus::DurabilityUnknown);
    CHECK(uncertain.durabilityUncertain());
    CHECK_EQ(
        uncertain.request(*fixture.host),
        CheckpointRequestStatus::Uncertain);
    CHECK_EQ(
        uncertain.recover(fixture.resources, fixture.generator).status,
        CheckpointRecoveryStatus::Corrupt);
}

TEST_CASE(SimulationCheckpoint_filesystem_reopens_the_acknowledged_generation) {
    HostFixture fixture;
    fixture.host->advance(17ms);
    const uint64_t expectedHash = fixture.host->stateHash();
    Test::TemporaryDirectory directory("rigel_simulation_checkpoint");
    const std::string root = (directory.path() / "world").string();
    {
        auto storage = std::make_shared<Persistence::FilesystemBackend>();
        SimulationCheckpointManager manager(storage, root);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(
            waitForCheckpoint(manager).status,
            CheckpointWriteStatus::Durable);
    }

    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    SimulationCheckpointManager reopened(storage, root);
    CHECK_EQ(
        reopened.request(*fixture.host),
        CheckpointRequestStatus::UnsupportedState);
    auto recovered = reopened.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.generation, uint64_t{1});
    CHECK_EQ(recovered.host->stateHash(), expectedHash);
}

TEST_CASE(SimulationCheckpoint_uncertain_filesystem_publication_survives_reopen) {
    HostFixture fixture;
    Test::TemporaryDirectory directory("rigel_checkpoint_uncertain");
    const std::string root = (directory.path() / "world").string();
    {
        auto faulted = std::make_shared<CheckpointFaultStorage>(
            CheckpointFaultStorage::Fault::PointerUncertain,
            std::make_shared<Persistence::FilesystemBackend>());
        SimulationCheckpointManager manager(faulted, root);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(waitForCheckpoint(manager).status,
                 CheckpointWriteStatus::DurabilityUnknown);
    }

    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    SimulationCheckpointManager reopened(storage, root);
    CHECK(reopened.durabilityUncertain());
    CHECK_EQ(reopened.request(*fixture.host), CheckpointRequestStatus::Uncertain);
    CHECK_EQ(reopened.recover(fixture.resources, fixture.generator).status,
             CheckpointRecoveryStatus::Corrupt);
    CHECK(storage->exists(root + "/publication-pending"));
}

TEST_CASE(SimulationCheckpoint_filesystem_failure_retry_and_orphan_are_safe) {
    HostFixture fixture;
    Test::TemporaryDirectory directory("rigel_checkpoint_retry");
    const std::string root = (directory.path() / "world").string();
    auto filesystem = std::make_shared<Persistence::FilesystemBackend>();
    auto faulted = std::make_shared<CheckpointFaultStorage>(
        CheckpointFaultStorage::Fault::PointerNotPublished, filesystem);
    {
        SimulationCheckpointManager manager(faulted, root);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(waitForCheckpoint(manager).status,
                 CheckpointWriteStatus::NotPublished);
        CHECK(!filesystem->exists(root + "/current"));
        CHECK(!filesystem->exists(root + "/publication-pending"));
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(waitForCheckpoint(manager).status,
                 CheckpointWriteStatus::Durable);
    }
    SimulationCheckpointManager reopened(filesystem, root);
    auto recovered = reopened.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.generation, uint64_t{1});
}

TEST_CASE(SimulationCheckpoint_teardown_joins_writer_before_releasing_root) {
    HostFixture fixture;
    Test::TemporaryDirectory directory("rigel_checkpoint_teardown");
    const std::string root = (directory.path() / "world").string();
    auto faulted = std::make_shared<CheckpointFaultStorage>(
        CheckpointFaultStorage::Fault::DelayPayload,
        std::make_shared<Persistence::FilesystemBackend>());
    auto manager = std::make_unique<SimulationCheckpointManager>(faulted, root);
    CHECK_EQ(manager->request(*fixture.host), CheckpointRequestStatus::Started);
    faulted->waitForPayload();
    auto teardown = std::async(std::launch::async,
        [owned = std::move(manager)]() mutable { owned.reset(); });
    CHECK_EQ(teardown.wait_for(20ms), std::future_status::timeout);
    faulted->releasePayload();
    CHECK_EQ(teardown.wait_for(2s), std::future_status::ready);

    auto filesystem = std::make_shared<Persistence::FilesystemBackend>();
    SimulationCheckpointManager reopened(filesystem, root);
    auto recovered = reopened.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    recovered.host->advance(17ms);
    CHECK_EQ(reopened.request(*recovered.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(reopened).generation, uint64_t{2});
    CHECK_EQ(reopened.recover(fixture.resources, fixture.generator).generation,
             uint64_t{2});
}

TEST_CASE(SimulationCheckpoint_full_saved_cut_hash_is_the_next_parent) {
    HostFixture fixture;
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/save");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    const auto first = waitForCheckpoint(manager);
    CHECK_EQ(first.status, CheckpointWriteStatus::Durable);
    const auto firstPayload = readStorageBytes(*storage, "/save/checkpoints/1.bin");
    const uint64_t firstCutHash = testHash(firstPayload);

    auto recovered = manager.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    const uint64_t comparisonHash = recovered.host->stateHash();
    CHECK_EQ(recovered.host->advance(1ms).ticksRun, size_t{0});
    CHECK_EQ(recovered.host->stateHash(), comparisonHash);
    CHECK_EQ(manager.request(*recovered.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    const auto pointer = readStorageBytes(*storage, "/save/current");
    CHECK_EQ(readBigU64(pointer, 16), firstCutHash);
    CHECK_NE(testHash(readStorageBytes(*storage, "/save/checkpoints/0.bin")),
             firstCutHash);
}

TEST_CASE(SimulationCheckpoint_fence_failures_preserve_the_acknowledged_payload) {
    for (const auto fault : {CheckpointFaultStorage::Fault::PendingUncertain,
                             CheckpointFaultStorage::Fault::RemovePending}) {
        HostFixture fixture;
        Test::TemporaryDirectory directory("rigel_checkpoint_fence");
        const std::string root = (directory.path() / "world").string();
        auto filesystem = std::make_shared<Persistence::FilesystemBackend>();
        std::vector<uint8_t> acknowledgedPayload;
        {
            auto storage = std::make_shared<CheckpointFaultStorage>(
                CheckpointFaultStorage::Fault::None, filesystem);
            SimulationCheckpointManager manager(storage, root);
            CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
            CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
            acknowledgedPayload = readStorageBytes(*filesystem, root + "/checkpoints/1.bin");
            fixture.host->advance(17ms);
            storage->setFault(fault);
            CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
            CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::DurabilityUnknown);
            CHECK(manager.durabilityUncertain());
            CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Uncertain);
        }
        SimulationCheckpointManager reopened(filesystem, root);
        CHECK(reopened.durabilityUncertain());
        CHECK_EQ(reopened.request(*fixture.host), CheckpointRequestStatus::Uncertain);
        CHECK_EQ(reopened.recover(fixture.resources, fixture.generator).status,
                 CheckpointRecoveryStatus::Corrupt);
        CHECK_EQ(readStorageBytes(*filesystem, root + "/checkpoints/1.bin"),
                 acknowledgedPayload);
        CHECK(filesystem->list(root + "/checkpoints").size() <= 2);
    }
}

TEST_CASE(SimulationCheckpoint_reuses_bounded_slots_through_success_and_failure) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {7, 7, 7}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 512;
    HostFixture fixture(config);
    Test::TemporaryDirectory directory("rigel_checkpoint_slots");
    const std::string root = (directory.path() / "world").string();
    auto filesystem = std::make_shared<Persistence::FilesystemBackend>();
    auto storage = std::make_shared<CheckpointFaultStorage>(
        CheckpointFaultStorage::Fault::None, filesystem);
    SimulationCheckpointManager manager(storage, root);
    for (uint64_t generation = 1; generation <= 12; ++generation) {
        fixture.host->advance(17ms);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        const auto saved = waitForCheckpoint(manager);
        CHECK_EQ(saved.status, CheckpointWriteStatus::Durable);
        CHECK_EQ(saved.generation, generation);
        CHECK(filesystem->list(root + "/checkpoints").size() <= 2);
    }
    const auto acknowledgedPointer = readStorageBytes(*filesystem, root + "/current");
    const auto acknowledgedPayload = readStorageBytes(*filesystem, root + "/checkpoints/0.bin");
    for (int failure = 0; failure < 12; ++failure) {
        fixture.host->advance(17ms);
        storage->setFault(CheckpointFaultStorage::Fault::PointerNotPublished);
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        const auto failed = waitForCheckpoint(manager);
        CHECK_EQ(failed.status, CheckpointWriteStatus::NotPublished);
        CHECK_EQ(failed.generation, uint64_t{13});
        CHECK_EQ(readStorageBytes(*filesystem, root + "/current"), acknowledgedPointer);
        CHECK_EQ(readStorageBytes(*filesystem, root + "/checkpoints/0.bin"), acknowledgedPayload);
        CHECK_EQ(filesystem->list(root + "/checkpoints").size(), size_t{2});
    }
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    auto recovered = manager.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.generation, uint64_t{13});
    CHECK_EQ(recovered.host->stateHash(), fixture.host->stateHash());
}

TEST_CASE(SimulationCheckpoint_preserves_incompatible_container_versions) {
    HostFixture fixture;
    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    {
        SimulationCheckpointManager manager(storage, "/save");
        CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
        CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    }
    auto format = readStorageBytes(*storage, "/save/format");
    writeBigU32(format, 4, 1);
    {
        auto write = storage->openWrite("/save/format");
        write->writer().writeBytes(format.data(), format.size());
        write->commit();
    }
    const auto pointer = readStorageBytes(*storage, "/save/current");
    const auto payload = readStorageBytes(*storage, "/save/checkpoints/1.bin");
    SimulationCheckpointManager incompatible(storage, "/save");
    CHECK_EQ(incompatible.request(*fixture.host), CheckpointRequestStatus::UnsupportedState);
    CHECK_EQ(incompatible.recover(fixture.resources, fixture.generator).status,
             CheckpointRecoveryStatus::Incompatible);
    CHECK_EQ(readStorageBytes(*storage, "/save/format"), format);
    CHECK_EQ(readStorageBytes(*storage, "/save/current"), pointer);
    CHECK_EQ(readStorageBytes(*storage, "/save/checkpoints/1.bin"), payload);
}

TEST_CASE(SimulationCheckpoint_incompatible_recovery_does_not_change_files) {
    HostFixture fixture;
    Test::TemporaryDirectory directory("rigel_checkpoint_identity");
    const std::string root = (directory.path() / "world").string();
    auto storage = std::make_shared<Persistence::FilesystemBackend>();
    SimulationCheckpointManager manager(storage, root);
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    const auto pointerBefore = readStorageBytes(*storage, root + "/current");
    const auto payloadBefore = readStorageBytes(*storage, root + "/checkpoints/1.bin");
    auto otherGenerator = std::make_shared<Voxel::WorldGenerator>(
        fixture.resources.registry(), flatDefinition(), 99);
    CHECK_EQ(manager.recover(fixture.resources, otherGenerator).status,
             CheckpointRecoveryStatus::Incompatible);
    CHECK_EQ(readStorageBytes(*storage, root + "/current"), pointerBefore);
    CHECK_EQ(readStorageBytes(*storage, root + "/checkpoints/1.bin"), payloadBefore);
}

TEST_CASE(SimulationCheckpoint_recovery_rejects_malformed_authority_state) {
    SimulationHostConfig config;
    config.maxPendingCommands = 2;
    HostFixture fixture(config);
    fixture.start();
    CHECK_EQ(fixture.host->submit(fixture.removeCommand(1)).status,
             SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->submit(fixture.removeCommand(2)).status,
             SubmitStatus::Accepted);
    auto extra = std::make_unique<Entity::Entity>();
    extra->addTag(Entity::EntityTags::NoClip);
    extra->setPosition({9, 7, 9});
    CHECK(!fixture.host->spawnEntity(std::move(extra)).isNull());

    auto rejects = [&](const auto& mutation) {
        CHECK_EQ(recoverMutatedCheckpoint(fixture, mutation),
                 CheckpointRecoveryStatus::Corrupt);
    };
    rejects([](auto& bytes, const auto& layout) {
        bytes.at(layout.receipts.at(0).action) = 255;
    });
    rejects([](auto& bytes, const auto& layout) {
        bytes.at(layout.receipts.at(0).direction) = 255;
    });
    rejects([](auto& bytes, const auto& layout) {
        std::copy_n(bytes.begin() + layout.receipts.at(0).command + 8, 8,
                    bytes.begin() + layout.receipts.at(1).command + 8);
    });
    rejects([](auto& bytes, const auto& layout) {
        std::copy_n(bytes.begin() + layout.receipts.at(0).admission, 8,
                    bytes.begin() + layout.receipts.at(1).admission);
    });
    rejects([](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.receipts.at(0).command, 2);
    });
    rejects([](auto& bytes, const auto& layout) {
        bytes.at(layout.receipts.at(0).pending) = 0;
    });
    rejects([](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.replayLimits.at(1), 1);
    });
    rejects([](auto& bytes, const auto& layout) {
        std::copy_n(bytes.begin() + layout.entities.at(0), 16,
                    bytes.begin() + layout.entities.at(1));
    });
    rejects([](auto& bytes, const auto& layout) {
        writeBigU32(bytes, layout.nextEntity, 1);
    });
    rejects([](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.replayLimits.at(5), 1);
    });
    rejects([](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.replayLimits.at(6), 32);
    });
    rejects([](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.replayLimits.at(7), 1);
    });
    rejects([](auto& bytes, const auto&) { bytes.pop_back(); });

    CHECK_EQ(fixture.host->advance(34ms).ticksRun, size_t{2});
    rejects([](auto& bytes, const auto& layout) {
        writeBigU64(bytes, layout.receipts.at(0).outcome, 2);
    });
}

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
TEST_CASE(SimulationCheckpoint_preflights_collection_counts_before_allocation) {
    for (int collection = 0; collection < 4; ++collection) {
        HostFixture fixture;
        fixture.start();
        CHECK_EQ(fixture.host->submit(fixture.removeCommand(1)).status,
                 SubmitStatus::Accepted);
        allocationCeilingExceeded = false;
        CheckpointRecoveryStatus status;
        try {
            status = recoverMutatedCheckpoint(fixture, [&](auto& bytes, const auto& layout) {
                constexpr uint32_t forgedCount = 1'000'000;
                if (collection == 0) {
                    writeBigU64(bytes, layout.replayLimits.at(3), forgedCount);
                    writeBigU32(bytes, layout.entities.front() - 4, forgedCount);
                } else if (collection == 1) {
                    writeBigU64(bytes, layout.replayLimits.at(4), forgedCount);
                    writeBigU32(bytes, layout.entityTagCounts.front(), forgedCount);
                } else if (collection == 2) {
                    writeBigU64(bytes, layout.replayLimits.at(0), forgedCount);
                    writeBigU32(bytes, layout.receipts.front().mutationCount, forgedCount);
                } else {
                    writeBigU32(bytes, layout.chunkCount, forgedCount);
                }
                // Intercept an unsafe request without allocating its advertised size.
                // Only recovery runs under this thread-local guard; the writer is joined.
                allocationCeiling = 8 * 1024 * 1024;
            });
        } catch (...) {
            allocationCeiling = 0;
            throw;
        }
        allocationCeiling = 0;
        if (status != CheckpointRecoveryStatus::Corrupt) {
            throw Test::TestFailure(
                "forged collection " + std::to_string(collection) +
                " was not diagnosed as corrupt");
        }
        CHECK(!allocationCeilingExceeded);
    }
}

TEST_CASE(SimulationCheckpoint_applies_current_sub_limits_before_decode_allocations) {
    SimulationHostConfig savedPolicy;
    savedPolicy.maxChangesPerCommand = 37;
    savedPolicy.maxEntityTags = 32;
    savedPolicy.maxEntityTagBytes = 4'096;
    savedPolicy.maxSnapshotCells = 25'000;
    HostFixture fixture(savedPolicy);
    fixture.start();

    EditCommand command{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
    };
    for (int index = 0; index < 37; ++index) {
        const CellAddress address{
            1 + index % 10, 7, 1 + index / 10};
        command.mutations.push_back({
            .address = address,
            .expected = fixture.host->read(address).state,
            .replacement = {"rigel:stone", 0},
        });
    }
    CHECK_EQ(fixture.host->submit(
                 command, fixture.host->authorityEditCapability()).status,
             SubmitStatus::Accepted);

    auto tagged = std::make_unique<Entity::Entity>();
    tagged->setPosition({9.0f, 7.0f, 9.0f});
    for (int index = 0; index < 23; ++index) {
        tagged->addTag("saved-tag:" + std::to_string(100 + index));
    }
    CHECK(!fixture.host->spawnEntity(std::move(tagged)).isNull());

    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/current-decode-limits");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    const auto pointer = readStorageBytes(
        *storage, "/current-decode-limits/current");
    const auto payload = readStorageBytes(
        *storage, "/current-decode-limits/checkpoints/1.bin");

    const auto rejectsBefore = [&](SimulationHostConfig current,
                                   size_t allocationSize) {
        watchedAllocationSize = allocationSize;
        watchedAllocationObserved = false;
        const auto recovery = manager.recover(
            fixture.resources, fixture.generator, std::move(current));
        watchedAllocationSize = 0;
        CHECK_EQ(recovery.status, CheckpointRecoveryStatus::Incompatible);
        CHECK(!watchedAllocationObserved);
        CHECK_EQ(readStorageBytes(
                     *storage, "/current-decode-limits/current"),
                 pointer);
        CHECK_EQ(readStorageBytes(
                     *storage,
                     "/current-decode-limits/checkpoints/1.bin"),
                 payload);
    };

    auto commandCount = savedPolicy;
    commandCount.maxChangesPerCommand = 1;
    rejectsBefore(commandCount, 37 * sizeof(CellMutation));

    auto commandBytes = savedPolicy;
    commandBytes.maxCommandBytes = 1'024;
    rejectsBefore(commandBytes, 37 * sizeof(CellMutation));

    auto tagCount = savedPolicy;
    tagCount.maxEntityTags = 1;
    rejectsBefore(tagCount, 23 * sizeof(std::string));

    auto tagBytes = savedPolicy;
    tagBytes.maxEntityTagBytes = 1'024;
    rejectsBefore(tagBytes, 23 * sizeof(std::string));
}
#endif

TEST_CASE(SimulationCheckpoint_preserves_pending_command_after_actor_removal) {
    HostFixture fixture;
    fixture.start();
    const auto command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    CHECK(fixture.host->despawnEntity(fixture.actor));
    SimulationCheckpointManager manager(
        std::make_shared<Persistence::InMemoryStorageBackend>(), "/save");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    SimulationHostConfig currentPolicy;
    currentPolicy.maxSnapshotCells = 25'000;
    auto restored = manager.recover(
        fixture.resources, fixture.generator, currentPolicy);
    CHECK_EQ(restored.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(restored.host->stateHash(), fixture.host->stateHash());
    restored.host->advance(17ms);
    fixture.host->advance(17ms);
    CHECK_EQ(restored.host->stateHash(), fixture.host->stateHash());
    CHECK_EQ(restored.host->submit(command).outcome->status,
             CommandOutcomeStatus::ActorUnavailable);
}

TEST_CASE(SimulationCheckpoint_live_root_owner_is_exclusive) {
    Test::TemporaryDirectory directory("rigel_checkpoint_owner");
    const std::string root = (directory.path() / "world").string();
    auto first = std::make_unique<SimulationCheckpointManager>(
        std::make_shared<Persistence::FilesystemBackend>(), root);
    std::promise<void> attempting;
    std::promise<void> acquired;
    auto attempted = attempting.get_future();
    auto completed = acquired.get_future();
    std::thread contender([&] {
        attempting.set_value();
        SimulationCheckpointManager second(
            std::make_shared<Persistence::FilesystemBackend>(), root);
        acquired.set_value();
    });
    attempted.wait();
    const bool blockedWhileOwned = completed.wait_for(20ms) ==
        std::future_status::timeout;
    first.reset();
    const bool acquiredAfterRelease = completed.wait_for(2s) ==
        std::future_status::ready;
    contender.join();
    CHECK(blockedWhileOwned);
    CHECK(acquiredAfterRelease);
}

TEST_CASE(SimulationHost_admits_edit_once_and_retains_session_receipts) {
    SimulationHostConfig config;
    config.maxPendingCommands = 1;
    config.maxSessionReceipts = 2;
    HostFixture fixture(config);
    fixture.start();
    EditCommand first = fixture.removeCommand(1);

    CHECK_EQ(fixture.host->submit(first).status, SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->submit(first).status, SubmitStatus::DuplicatePending);
    EditCommand conflict = first;
    conflict.mutations.front().replacement.metadata = 1;
    CHECK_EQ(fixture.host->submit(conflict).status, SubmitStatus::PayloadConflict);
    EditCommand queued = fixture.removeCommand(2);
    CHECK_EQ(fixture.host->submit(queued).status, SubmitStatus::CommandCapacity);

    fixture.host->advance(17ms);
    const auto duplicate = fixture.host->submit(first);
    CHECK_EQ(duplicate.status, SubmitStatus::DuplicateComplete);
    CHECK_EQ(duplicate.outcome->status, CommandOutcomeStatus::Applied);
    CHECK_EQ(
        fixture.host->read(first.mutations.front().address).state.blockKey,
        std::string("base:air"));

    CHECK_EQ(fixture.host->submit(queued).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(queued).outcome->status,
        CommandOutcomeStatus::TargetMismatch);
    EditCommand overCapacity = queued;
    overCapacity.command = 3;
    CHECK_EQ(
        fixture.host->submit(overCapacity).status,
        SubmitStatus::ReceiptCapacity);

    CHECK_EQ(
        fixture.host->startSession(2, fixture.actor, fixture.host->content().identity()),
        SessionStartStatus::Started);
    CHECK_EQ(fixture.host->submit(first).status, SubmitStatus::OldSession);
    CHECK_EQ(
        fixture.host->startSession(1, fixture.actor, fixture.host->content().identity()),
        SessionStartStatus::OldSession);
}

TEST_CASE(SimulationHost_rejects_oversized_retained_command_payloads) {
    SimulationHostConfig config;
    config.maxCommandBytes = 1024;
    HostFixture fixture(config);
    fixture.start();
    auto command = fixture.removeCommand(1);
    const auto address = command.mutations.front().address;
    command.mutations.front().replacement.blockKey.reserve(4096);
    CHECK_EQ(
        fixture.host->submit(std::move(command)).status,
        SubmitStatus::CommandCapacity);
    CHECK_NE(
        fixture.host->read(address).state.blockKey,
        std::string("base:air"));
}

TEST_CASE(SimulationHost_orders_racing_edits_against_authoritative_state) {
    HostFixture fixture;
    fixture.start();
    const EditCommand first = fixture.removeCommand(1);
    EditCommand second = first;
    second.command = 2;
    CHECK_EQ(fixture.host->submit(first).status, SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->submit(second).status, SubmitStatus::Accepted);

    fixture.host->advance(17ms);
    fixture.host->advance(17ms);
    const auto firstResult = fixture.host->submit(first);
    const auto secondResult = fixture.host->submit(second);
    CHECK_EQ(firstResult.outcome->status, CommandOutcomeStatus::Applied);
    CHECK_EQ(secondResult.outcome->status, CommandOutcomeStatus::TargetMismatch);
    CHECK(firstResult.outcome->admission < secondResult.outcome->admission);
}

TEST_CASE(SimulationHost_interaction_cannot_cross_unavailable_terrain) {
    SimulationHostConfig config;
    config.domain = {{0, -4, 0}, {63, 8, 31}};
    config.preloadedChunks = {{0, -1, 0}, {0, 0, 0}};
    config.maxPreloadedChunks = 2;
    config.maxSnapshotCells = 30'000;
    HostFixture fixture(config, {31.5f, 6.0f, 5.5f});
    fixture.start();
    const CellAddress target = fixture.surface(31, 5);
    EditCommand command = fixture.removeCommand(1);
    command.interaction->origin = {31.5f, 6.0f, 5.5f};
    command.interaction->expectedTarget = target;
    command.interaction->expectedTargetState = fixture.host->read(target).state;
    command.mutations.front().address = target;
    command.mutations.front().expected = fixture.host->read(target).state;
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(command).outcome->status,
        CommandOutcomeStatus::Unavailable);
    CHECK_NE(fixture.host->read(target).state.blockKey, std::string("base:air"));
}

TEST_CASE(SimulationHost_ignores_unavailable_terrain_behind_a_known_hit) {
    SimulationHostConfig config;
    config.domain = {{0, -32, 0}, {31, 8, 31}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 50'000;
    config.maxInteractionDistance = 20.0f;
    HostFixture fixture(config);
    fixture.start();
    EditCommand command = fixture.removeCommand(1);
    command.interaction->maxDistance = 20.0f;
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(command).outcome->status,
        CommandOutcomeStatus::Applied);
}

TEST_CASE(SimulationHost_rejects_content_and_shape_invalid_interactions) {
    HostFixture fixture({}, {5.5f, 1.5f, 5.5f});
    fixture.start();
    EditCommand place = fixture.removeCommand(1);
    place.action = EditAction::Place;
    const auto target = place.interaction->expectedTarget;
    place.mutations.front() = {
        {target.x, target.y + 1, target.z},
        {"base:air", 0},
        {"rigel:stone", 0},
    };
    CHECK_EQ(fixture.host->submit(place).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(place).outcome->status,
        CommandOutcomeStatus::PlacementCollision);
    CHECK_EQ(
        fixture.host->read(place.mutations.front().address).state.blockKey,
        std::string("base:air"));

    EditCommand mismatch = place;
    mismatch.command = 2;
    mismatch.content = {};
    CHECK_EQ(
        fixture.host->submit(std::move(mismatch)).status,
        SubmitStatus::ContentMismatch);
}

TEST_CASE(SimulationHost_places_on_the_admitted_tick_and_publishes_destination) {
    HostFixture fixture;
    fixture.start();
    EditCommand place = fixture.removeCommand(1);
    place.action = EditAction::Place;
    const CellAddress target = place.interaction->expectedTarget;
    const CellAddress destination{target.x, target.y + 1, target.z};
    place.mutations.front() = {
        destination,
        {"base:air", 0},
        {"rigel:stone", 0},
    };

    auto connection = fixture.host->connectReplica({destination, destination});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);
    CHECK_EQ(replica.read(destination).state.blockKey, std::string("base:air"));

    CHECK_EQ(fixture.host->submit(place).status, SubmitStatus::Accepted);
    CHECK_EQ(
        fixture.host->read(destination).state.blockKey,
        std::string("base:air"));
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Idle);

    CHECK_EQ(fixture.host->advance(17ms).ticksRun, static_cast<size_t>(1));
    const auto completed = fixture.host->submit(place);
    CHECK_EQ(completed.status, SubmitStatus::DuplicateComplete);
    CHECK_EQ(completed.outcome->status, CommandOutcomeStatus::Applied);
    CHECK_EQ(completed.outcome->tick, Tick{1});
    CHECK_EQ(
        fixture.host->read(destination).state.blockKey,
        std::string("rigel:stone"));
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(replica.read(destination).state.blockKey,
             std::string("rigel:stone"));
}

TEST_CASE(SimulationHost_rejects_unrepresentable_air_without_publication) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {31, 30, 31}};
    config.preloadedChunks = {{0, 0, 0}};
    config.maxPreloadedChunks = 1;
    config.maxSnapshotCells = 32'768;
    HostFixture fixture(config);
    fixture.start();
    const CellAddress address{5, 20, 5};
    const SemanticBlockState air = fixture.host->read(address).state;
    CHECK_EQ(air, (SemanticBlockState{"base:air", 0}));

    auto connection = fixture.host->connectReplica({address, address});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);

    EditCommand unsupported{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
        .mutations = {{address, air, {"base:air", 1}}},
    };
    const auto capability = fixture.host->authorityEditCapability();
    CHECK_EQ(
        fixture.host->submit(unsupported, capability).status,
        SubmitStatus::InvalidRequest);
    CHECK_EQ(fixture.host->revision(), Revision{0});
    CHECK_EQ(fixture.host->read(address).state, air);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Idle);
    CHECK_EQ(replica.read(address).state, air);

    EditCommand supported = unsupported;
    supported.command = 2;
    supported.mutations.front().replacement = {"rigel:stone", 1};
    CHECK_EQ(
        fixture.host->submit(supported, capability).status,
        SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(fixture.host->read(address).state,
             supported.mutations.front().replacement);
    CHECK_EQ(replica.read(address).state,
             supported.mutations.front().replacement);
}

TEST_CASE(SimulationHost_keeps_packed_light_out_of_semantic_edits_and_hashes) {
    HostFixture fixture;
    fixture.start();
    const EditCommand command = fixture.removeCommand(2);
    const CellAddress target = command.mutations.front().address;
    const auto before = fixture.host->read(target);
    const uint64_t lightIndependentHash = fixture.host->stateHash();

    auto& world = const_cast<Voxel::World&>(fixture.host->world());
    world.setBlock(
        target.x, target.y, target.z,
        fixture.host->content().localState(before.state, 0xa3));
    const auto relit = fixture.host->read(target);
    CHECK_EQ(relit.state, before.state);
    CHECK_EQ(relit.lightLevel, uint8_t{0xa3});
    CHECK_EQ(fixture.host->stateHash(), lightIndependentHash);

    auto connection = fixture.host->connectReplica({target, target});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);
    CHECK_EQ(replica.read(target).state, before.state);
    CHECK_EQ(replica.read(target).lightLevel, uint8_t{0xa3});

    EditCommand unchanged{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
        .mutations = {{target, before.state, before.state}},
    };
    CHECK_EQ(fixture.host->submit(
        unchanged, fixture.host->authorityEditCapability()).status,
        SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(fixture.host->read(target).lightLevel, uint8_t{0xa3});
    const uint64_t checkpointHash = fixture.host->stateHash();

    auto storage = std::make_shared<Persistence::InMemoryStorageBackend>();
    SimulationCheckpointManager manager(storage, "/light-checkpoint");
    CHECK_EQ(manager.request(*fixture.host), CheckpointRequestStatus::Started);
    CHECK_EQ(waitForCheckpoint(manager).status, CheckpointWriteStatus::Durable);
    auto recovered = manager.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovered.status, CheckpointRecoveryStatus::Recovered);
    CHECK_EQ(recovered.host->read(target).state, before.state);
    CHECK_EQ(recovered.host->read(target).lightLevel, uint8_t{0xa3});
    CHECK_EQ(recovered.host->stateHash(), checkpointHash);

    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(command).outcome->status,
        CommandOutcomeStatus::Applied);
}

TEST_CASE(SimulationHost_preserves_light_for_unchanged_atomic_members) {
    HostFixture fixture;
    fixture.start();
    const auto retained = fixture.surface();
    const auto removed = fixture.surface(6, 5);
    auto& world = const_cast<Voxel::World&>(fixture.host->world());
    const auto retainedState = fixture.host->read(retained).state;
    world.setBlock(retained.x, retained.y, retained.z,
                   fixture.host->content().localState(retainedState, 0x73));
    EditCommand command{
        .session = 1, .command = 1, .actor = fixture.actor,
        .world = 0, .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
        .mutations = {
            {retained, retainedState, retainedState},
            {removed, fixture.host->read(removed).state, {"base:air", 0}},
        },
    };
    CHECK_EQ(fixture.host->submit(command, fixture.host->authorityEditCapability()).status,
             SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    const auto repeated = fixture.host->submit(command, fixture.host->authorityEditCapability());
    CHECK(repeated.outcome.has_value());
    CHECK_EQ(repeated.outcome->status, CommandOutcomeStatus::Applied);
    CHECK_EQ(fixture.host->read(removed).state.blockKey, std::string("base:air"));
    CHECK_EQ(fixture.host->read(retained).state, retainedState);
    CHECK_EQ(fixture.host->read(retained).lightLevel, uint8_t{0x73});
}

TEST_CASE(SimulationRecording_reconstructs_transitional_light_separately) {
    HostFixture fixture;
    SimulationHostConfig config;
    config.domain = {{-4, -4, -4}, {20, 8, 20}};
    config.maxPreloadedChunks = 8;
    config.maxSnapshotCells = 25'000;
    SimulationHost host(fixture.resources, fixture.generator, config);
    const CellAddress target{5, 0, 5};
    const auto before = host.read(target);
    CHECK_EQ(before.status, ExactReadStatus::Known);
    auto& world = const_cast<Voxel::World&>(host.world());
    world.setBlock(
        target.x, target.y, target.z,
        host.content().localState(before.state, 0x4d));

    const auto recording = host.recording();
    CHECK(recording.has_value());
    auto replayed = SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {17ms});
    CHECK_EQ(replayed.status, ResimulationStatus::Complete);
    CHECK(replayed.host != nullptr);
    CHECK_EQ(replayed.host->read(target).state, before.state);
    CHECK_EQ(replayed.host->read(target).lightLevel, uint8_t{0x4d});
}

TEST_CASE(SimulationHost_rejects_unbound_and_nonfinite_interactions) {
    {
        HostFixture fixture;
        fixture.start();
        auto command = fixture.removeCommand(1);
        const auto address = command.mutations.front().address;
        const auto before = fixture.host->read(address).state;
        command.interaction->origin = {6.5f, 6.0f, 5.5f};
        CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
        fixture.host->advance(17ms);
        CHECK_EQ(
            fixture.host->submit(command).outcome->status,
            CommandOutcomeStatus::InvalidRequest);
        CHECK_EQ(fixture.host->read(address).state, before);
    }

    for (const glm::vec3 origin : {
             glm::vec3{std::numeric_limits<float>::quiet_NaN(), 6.0f, 5.5f},
             glm::vec3{std::numeric_limits<float>::infinity(), 6.0f, 5.5f}}) {
        HostFixture nonfinite;
        nonfinite.start();
        auto command = nonfinite.removeCommand(1);
        const auto address = command.mutations.front().address;
        const auto before = nonfinite.host->read(address).state;
        command.interaction->origin = origin;
        CHECK_EQ(
            nonfinite.host->submit(command).status,
            SubmitStatus::InvalidRequest);
        CHECK_EQ(nonfinite.host->read(address).state, before);
    }

    HostFixture fixture;
    fixture.start();
    auto tooFar = fixture.removeCommand(2);
    const auto before = fixture.host->read(tooFar.mutations.front().address).state;
    tooFar.interaction->maxDistance = 9.0f;
    CHECK_EQ(fixture.host->submit(tooFar).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(tooFar).outcome->status,
        CommandOutcomeStatus::InvalidRequest);
    CHECK_EQ(fixture.host->read(tooFar.mutations.front().address).state, before);

    auto unrepresentable = fixture.removeCommand(3);
    unrepresentable.interaction->direction = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        0.0f,
    };
    CHECK_EQ(
        fixture.host->submit(unrepresentable).status,
        SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(unrepresentable).outcome->status,
        CommandOutcomeStatus::InvalidRequest);
    CHECK_EQ(
        fixture.host->read(unrepresentable.mutations.front().address).state,
        before);
}

TEST_CASE(SimulationHost_rejects_unknown_action_values) {
    HostFixture fixture;
    fixture.start();
    auto command = fixture.removeCommand(1);
    const auto address = command.mutations.front().address;
    const auto before = fixture.host->read(address).state;
    command.action = static_cast<EditAction>(255);
    CHECK_EQ(
        fixture.host->submit(command).status,
        SubmitStatus::InvalidRequest);
    CHECK_EQ(fixture.host->read(address).state, before);

    auto invalidFace = fixture.removeCommand(2);
    invalidFace.interaction->expectedFace = static_cast<Voxel::Direction>(255);
    CHECK_EQ(fixture.host->submit(invalidFace).status,
             SubmitStatus::InvalidRequest);
    CHECK_EQ(fixture.host->read(address).state, before);
}

TEST_CASE(SimulationHost_bulk_edits_require_host_bound_authority) {
    SimulationHostConfig config;
    config.domain = {{0, 0, 0}, {63, 7, 7}};
    config.preloadedChunks = {{0, 0, 0}, {1, 0, 0}};
    config.maxPreloadedChunks = 2;
    config.maxSnapshotCells = 8'000;
    HostFixture fixture(config, {1.5f, 6.0f, 1.5f});
    fixture.start();

    const CellAddress first{31, 7, 1};
    const CellAddress second{32, 7, 1};
    EditCommand command{
        .session = 1,
        .command = 1,
        .actor = fixture.actor,
        .world = 0,
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = EditAction::Atomic,
        .mutations = {
            {first, {"base:air", 0}, {"rigel:stone", 0}},
            {second, {"rigel:stone", 0}, {"rigel:stone", 0}},
        },
    };
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::InvalidRequest);
    CHECK_EQ(fixture.host->read(first).state.blockKey, std::string("base:air"));

    HostFixture other;
    const auto wrongCapability = other.host->authorityEditCapability();
    CHECK_EQ(
        fixture.host->submit(command, wrongCapability).status,
        SubmitStatus::InvalidRequest);

    const auto capability = fixture.host->authorityEditCapability();
    CHECK_EQ(
        fixture.host->submit(command, capability).status,
        SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(command, capability).outcome->status,
        CommandOutcomeStatus::StaleState);
    CHECK_EQ(fixture.host->read(first).state.blockKey, std::string("base:air"));
    CHECK_EQ(fixture.host->read(second).state.blockKey, std::string("base:air"));
}

namespace {
class ThrowingHostEntity final : public Entity::Entity {
public:
    void update(Voxel::World&, float) override {
        throw Test::TestFailure("unsupported update ran");
    }
};
}

TEST_CASE(SimulationHost_rejects_entity_rules_outside_its_manifest) {
    HostFixture fixture;
    auto throwing = std::make_unique<ThrowingHostEntity>();
    throwing->setPosition({5.5f, 6.0f, 5.5f});
    CHECK(fixture.host->spawnEntity(std::move(throwing)).isNull());

    auto customHitbox = std::make_unique<Entity::Entity>();
    customHitbox->setLocalBounds({{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
    CHECK(fixture.host->spawnEntity(std::move(customHitbox)).isNull());

    fixture.start();
    const auto command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->advance(17ms).ticksRun, static_cast<size_t>(1));
    CHECK_EQ(
        fixture.host->submit(command).outcome->status,
        CommandOutcomeStatus::Applied);
}

TEST_CASE(SimulationHost_validates_pending_entity_acceleration_before_admission) {
    HostFixture fixture;
    const auto count = fixture.host->world().entities().size();
    for (int axis = 0; axis < 3; ++axis) {
        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity()}) {
            auto entity = std::make_unique<Entity::Entity>();
            entity->setPosition({5.5f, 6.0f, 5.5f});
            glm::vec3 acceleration{0.0f};
            acceleration[axis] = invalid;
            entity->accelerate(acceleration);
            CHECK(fixture.host->spawnEntity(std::move(entity)).isNull());
        }
    }
    CHECK_EQ(fixture.host->world().entities().size(), count);
    auto valid = std::make_unique<Entity::Entity>();
    valid->setPosition({5.5f, 6.0f, 5.5f});
    valid->addTag(Entity::EntityTags::NoClip);
    valid->accelerate({3.0f, 0.0f, 0.0f});
    const auto id = fixture.host->spawnEntity(std::move(valid));
    CHECK(!id.isNull());
    CHECK_EQ(fixture.host->world().entities().get(id)->acceleration(),
             glm::vec3(3.0f, 0.0f, 0.0f));
    fixture.host->advance(17ms);
    CHECK_NEAR(fixture.host->world().entities().get(id)->velocity().x,
               3.0f / 60.0f, 0.00001f);
    CHECK_EQ(fixture.host->world().entities().get(id)->acceleration(), glm::vec3(0));
}

TEST_CASE(SimulationHost_bounds_and_removes_owned_entities) {
    SimulationHostConfig config;
    config.maxEntities = 2;
    config.maxEntityTags = 2;
    config.maxEntityTagBytes = 256;
    HostFixture fixture(config);
    fixture.start();

    auto second = std::make_unique<Entity::Entity>();
    second->addTag(Entity::EntityTags::Passive);
    const Entity::EntityId secondId = fixture.host->spawnEntity(std::move(second));
    CHECK(!secondId.isNull());
    CHECK_EQ(fixture.host->world().entities().size(), static_cast<size_t>(2));
    CHECK(fixture.host->spawnEntity(
        std::make_unique<Entity::Entity>()).isNull());

    CHECK(fixture.host->despawnEntity(secondId));
    CHECK(!fixture.host->despawnEntity(secondId));
    CHECK_EQ(fixture.host->world().entities().size(), static_cast<size_t>(1));

    auto tooManyTags = std::make_unique<Entity::Entity>();
    tooManyTags->addTag("one");
    tooManyTags->addTag("two");
    tooManyTags->addTag("three");
    CHECK(fixture.host->spawnEntity(std::move(tooManyTags)).isNull());

    auto oversizedTag = std::make_unique<Entity::Entity>();
    oversizedTag->addTag(std::string(256, 't'));
    CHECK(fixture.host->spawnEntity(std::move(oversizedTag)).isNull());

    const auto replacement = fixture.host->spawnEntity(
        std::make_unique<Entity::Entity>());
    CHECK(!replacement.isNull());
    CHECK_NE(replacement, secondId);

    const EditCommand command = fixture.removeCommand(1);
    CHECK(fixture.host->despawnEntity(fixture.actor));
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(
        fixture.host->submit(command).outcome->status,
        CommandOutcomeStatus::ActorUnavailable);
    CHECK_EQ(fixture.host->world().entities().size(), static_cast<size_t>(1));
}

TEST_CASE(SimulationHost_rejects_overretained_config_and_entity_storage) {
    {
        SimulationHostConfig config;
        config.zone.reserve(config.maxCommandBytes + 1);
        CHECK_THROWS(HostFixture(std::move(config)));
    }
    {
        SimulationHostConfig config;
        config.preloadedChunks = {{0, 0, 0}};
        config.preloadedChunks.reserve(config.maxPreloadedChunks + 1);
        CHECK_THROWS(HostFixture(std::move(config)));
    }

    HostFixture fixture;
    auto churnedTags = std::make_unique<Entity::Entity>();
    for (size_t index = 0; index < 256; ++index) {
        churnedTags->addTag("churn:" + std::to_string(index));
    }
    for (size_t index = 0; index < 256; ++index) {
        churnedTags->removeTag("churn:" + std::to_string(index));
    }
    CHECK_EQ(churnedTags->tags().size(), static_cast<size_t>(0));
    CHECK(fixture.host->spawnEntity(std::move(churnedTags)).isNull());

    std::string retainedType;
    retainedType.reserve(4096);
    retainedType = "rigel:entity";
    CHECK(fixture.host->spawnEntity(
        std::make_unique<Entity::Entity>(std::move(retainedType))).isNull());

    auto retainedEmptyModel = std::make_unique<Entity::Entity>();
    std::string emptyModel;
    emptyModel.reserve(4096);
    retainedEmptyModel->setModelIdentifier(std::move(emptyModel));
    CHECK(fixture.host->spawnEntity(std::move(retainedEmptyModel)).isNull());

    auto retainedEmptyHandle = std::make_unique<Entity::Entity>();
    std::string emptyHandleId;
    emptyHandleId.reserve(4096);
    retainedEmptyHandle->setModel(Asset::Handle<Entity::EntityModelAsset>(
        {}, std::move(emptyHandleId)));
    CHECK(fixture.host->spawnEntity(std::move(retainedEmptyHandle)).isNull());
}

TEST_CASE(SimulationHost_fixed_tick_is_render_pacing_independent_and_retains_debt) {
    auto run = [](int frames) {
        HostFixture fixture({}, {5.5f, 6.0f, 5.5f}, {1.0f, 0.0f, 0.0f});
        uint64_t previous = 0;
        for (int frame = 1; frame <= frames; ++frame) {
            const uint64_t elapsed =
                (1'000'000'000ULL * frame) / frames - previous;
            previous += elapsed;
            fixture.host->advance(std::chrono::nanoseconds(elapsed));
        }
        return std::pair{
            fixture.host->tick(),
            fixture.host->world().entities().get(fixture.actor)->position().x};
    };
    const auto at30 = run(30);
    const auto at60 = run(60);
    const auto at144 = run(144);
    CHECK_EQ(at30.first, Tick{60});
    CHECK_EQ(at60.first, Tick{60});
    CHECK_EQ(at144.first, Tick{60});
    CHECK_NEAR(at30.second, at60.second, 0.00001f);
    CHECK_NEAR(at60.second, at144.second, 0.00001f);

    HostFixture overloaded;
    const auto initial = overloaded.host->advance(1s);
    CHECK_EQ(initial.ticksRun, static_cast<size_t>(8));
    CHECK(initial.timeDebtRemaining);
    size_t drained = initial.ticksRun;
    while (drained < 60) {
        const auto next = overloaded.host->advance(0ns);
        CHECK(next.ticksRun > 0);
        drained += next.ticksRun;
    }
    CHECK_EQ(overloaded.host->tick(), Tick{60});
}

TEST_CASE(LoopbackReplica_baseline_delta_gap_and_resnapshot_are_bounded) {
    SimulationHostConfig config;
    config.maxReplicaQueue = 2;
    config.maxReplicas = 2;
    HostFixture fixture(config);
    fixture.start();
    const CellBounds interest{{3, -1, 3}, {7, 3, 7}};
    auto firstConnection = fixture.host->connectReplica(interest);
    auto secondConnection = fixture.host->connectReplica(interest);
    CHECK_EQ(firstConnection.status, ReplicaConnectStatus::Connected);
    CHECK_EQ(secondConnection.status, ReplicaConnectStatus::Connected);
    CHECK_EQ(
        fixture.host->connectReplica(interest).status,
        ReplicaConnectStatus::Capacity);
    auto first = std::move(*firstConnection.replica);
    auto slow = std::move(*secondConnection.replica);
    pumpBaseline(first);
    pumpBaseline(slow);

    const EditCommand command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(first.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(
        first.read(command.mutations.front().address).state.blockKey,
        std::string("base:air"));

    fixture.host->advance(17ms);
    CHECK_EQ(first.pumpOne(), ReplicaPumpStatus::Applied);
    fixture.host->advance(17ms);
    CHECK(slow.needsResnapshot());
    CHECK_EQ(slow.queuedMessages(), static_cast<size_t>(0));
    CHECK_EQ(slow.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
    CHECK_EQ(fixture.host->resnapshot(slow), ReplicaConnectStatus::Connected);
    CHECK_EQ(slow.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(slow.revision(), fixture.host->revision());
    CHECK_EQ(first.pumpOne(), ReplicaPumpStatus::Applied);

    WorldChangeBatch future{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = first.revision() + 1,
        .revision = first.revision() + 2,
        .tick = first.tick() + 1,
    };
    CHECK_EQ(
        first.accept(std::make_shared<const PublicationMessage>(future)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(first.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
}

TEST_CASE(LoopbackReplica_delivers_validated_authoritative_outcomes) {
    HostFixture fixture;
    fixture.start();
    const CellAddress target = fixture.surface();
    auto connection = fixture.host->connectReplica({target, target});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);

    const EditCommand command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    const auto authoritative = fixture.host->submit(command).outcome;
    CHECK(authoritative.has_value());
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    const WorldChangeBatch repeated{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision() - 1,
        .revision = replica.revision(),
        .tick = replica.tick(),
        .changes = {{target, fixture.host->read(target).state}},
        .outcomes = {*authoritative},
    };
    CHECK_EQ(replica.accept(std::make_shared<const PublicationMessage>(repeated)),
             ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Duplicate);
    CHECK(!replica.needsResnapshot());
    CHECK_EQ(replica.takeOutcome(), authoritative);
    CHECK(!replica.takeOutcome().has_value());

    WorldChangeBatch malformed{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
        .outcomes = {{
            .session = 1,
            .command = 2,
            .admission = 2,
            .tick = replica.tick(),
            .revision = replica.revision() + 1,
            .status = CommandOutcomeStatus::NoChange,
        }},
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(malformed)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
    CHECK(!replica.takeOutcome().has_value());
}

TEST_CASE(LoopbackReplica_survives_authority_and_resource_teardown_with_bound_state) {
    std::optional<LoopbackReplica> survivor;
    std::optional<CommandOutcome> authoritative;
    CellAddress target;
    {
        HostFixture fixture;
        fixture.start();
        target = fixture.surface();
        auto connection = fixture.host->connectReplica({target, target});
        CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
        auto replica = std::move(*connection.replica);
        pumpBaseline(replica);

        const EditCommand command = fixture.removeCommand(1);
        CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
        fixture.host->advance(17ms);
        authoritative = fixture.host->submit(command).outcome;
        CHECK(authoritative.has_value());
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
        survivor.emplace(std::move(replica));
    }

    CHECK_EQ(survivor->read(target).status, ExactReadStatus::Known);
    const SemanticBlockState air{"base:air", 0};
    CHECK_EQ(survivor->read(target).state, air);
    CHECK_EQ(survivor->takeOutcome(), authoritative);
    CHECK(!survivor->takeOutcome().has_value());
}

TEST_CASE(LoopbackReplica_owns_publications_independently_of_mutable_sender_aliases) {
    SimulationHostConfig config;
    config.maxReplicaBytes = 64 * 1024;
    HostFixture fixture(config);
    const CellAddress address{5, 0, 5};
    auto connection = fixture.host->connectReplica({address, address});
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);
    const SemanticBlockState expected{"rigel:water", 0};
    auto sender = std::make_shared<PublicationMessage>(WorldChangeBatch{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
        .changes = {{address, expected}},
    });
    const auto ownersBefore = sender.use_count();
    CHECK_EQ(replica.accept(sender), ReplicaAcceptStatus::Queued);
    CHECK_EQ(sender.use_count(), ownersBefore);
    auto& changed = std::get<WorldChangeBatch>(*sender);
    changed.content = {};
    changed.zone.assign(config.maxReplicaBytes * 2, 'x');
    changed.changes.front().state = {"base:air", 1};
    changed.changes.front().lightLevel = 7;
    changed.changes.reserve(config.maxReplicaBytes);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(replica.read(address).state, expected);
    CHECK(!replica.needsResnapshot());
}

TEST_CASE(LoopbackReplica_queued_authority_messages_survive_all_producer_teardown) {
    for (const bool pumpInitialBaseline : {false, true}) {
        std::optional<LoopbackReplica> survivor;
        std::optional<CommandOutcome> expectedOutcome;
        CellAddress target;
        SemanticBlockState expected;
        {
            HostFixture fixture;
            fixture.start();
            target = fixture.surface();
            auto connection = fixture.host->connectReplica({target, target});
            survivor.emplace(std::move(*connection.replica));
            if (pumpInitialBaseline) pumpBaseline(*survivor);
            const auto command = fixture.removeCommand(1);
            CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
            fixture.host->advance(17ms);
            expectedOutcome = fixture.host->submit(command).outcome;
            expected = fixture.host->read(target).state;
            CHECK_EQ(survivor->queuedMessages(), pumpInitialBaseline ? size_t{1} : size_t{2});
        }
        if (!pumpInitialBaseline) CHECK_EQ(survivor->pumpOne(), ReplicaPumpStatus::Applied);
        CHECK_EQ(survivor->pumpOne(), ReplicaPumpStatus::Applied);
        CHECK_EQ(survivor->read(target).state, expected);
        CHECK_EQ(survivor->takeOutcome(), expectedOutcome);
        CHECK_EQ(survivor->pumpOne(), ReplicaPumpStatus::Idle);
    }
}

TEST_CASE(SimulationHost_reclaims_disconnected_replica_capacity) {
    SimulationHostConfig config;
    config.maxReplicas = 1;
    HostFixture fixture(config);
    const auto target = fixture.surface();
    {
        auto connection = fixture.host->connectReplica({target, target});
        CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
        CHECK_EQ(fixture.host->connectReplica({target, target}).status,
                 ReplicaConnectStatus::Capacity);
    }
    auto replacement = fixture.host->connectReplica({target, target});
    CHECK_EQ(replacement.status, ReplicaConnectStatus::Connected);
    pumpBaseline(*replacement.replica);
    CHECK_EQ(replacement.replica->read(target).state, fixture.host->read(target).state);
}

TEST_CASE(LoopbackReplica_rejects_incomplete_and_oversized_messages) {
    SimulationHostConfig config;
    config.maxChangesPerCommand = 2;
    HostFixture fixture(config);
    auto connection = fixture.host->connectReplica({{3, -1, 3}, {7, 3, 7}});
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);
    fixture.host->advance(17ms);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);

    WorldChangeBatch duplicate{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision() - 1,
        .revision = replica.revision(),
        .tick = replica.tick(),
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(duplicate)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Duplicate);

    WorldChangeBatch oversized{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
    };
    oversized.changes.resize(3);
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(oversized)),
        ReplicaAcceptStatus::RejectedOversized);
    CHECK(replica.needsResnapshot());

    CHECK_EQ(fixture.host->resnapshot(replica), ReplicaConnectStatus::Connected);
    pumpBaseline(replica);
    WorldChangeBatch incomplete{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
        .complete = false,
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(incomplete)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);

    CHECK_EQ(fixture.host->resnapshot(replica), ReplicaConnectStatus::Connected);
    pumpBaseline(replica);
    incomplete.complete = true;
    incomplete.content = {};
    incomplete.baseRevision = replica.revision();
    incomplete.revision = replica.revision() + 1;
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(incomplete)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);

    CHECK_EQ(
        fixture.host->connectReplica({{-5, 0, 0}, {1, 1, 1}}).status,
        ReplicaConnectStatus::InvalidInterest);
}

TEST_CASE(LoopbackReplica_rejects_regressing_publication_ticks) {
    const CellAddress address{5, 0, 5};
    const CellBounds interest{address, address};

    for (const Tick tickRegression : {Tick{0}, Tick{1}}) {
        HostFixture fixture;
        auto connection = fixture.host->connectReplica(interest);
        auto replica = std::move(*connection.replica);
        pumpBaseline(replica);
        fixture.host->advance(34ms);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
        const auto previousRevision = replica.revision();
        const auto previousTick = replica.tick();

        WorldBaseline duplicate{
            .content = fixture.host->content().identity(),
            .world = 0,
            .zone = "base:default",
            .bounds = interest,
            .tick = previousTick,
            .revision = previousRevision,
            .cells = {{address, fixture.host->read(address).state}},
        };
        CHECK_EQ(
            replica.accept(std::make_shared<const PublicationMessage>(duplicate)),
            ReplicaAcceptStatus::Queued);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Duplicate);

        duplicate.revision = previousRevision + 1;
        duplicate.tick = previousTick - tickRegression;
        duplicate.cells.front().state = {"rigel:water", 0};
        CHECK_EQ(
            replica.accept(std::make_shared<const PublicationMessage>(duplicate)),
            ReplicaAcceptStatus::Queued);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
        CHECK_EQ(replica.revision(), previousRevision);
        CHECK_EQ(replica.tick(), previousTick);
        CHECK_EQ(replica.read(address).status, ExactReadStatus::Unavailable);
    }

    {
        HostFixture fixture;
        auto connection = fixture.host->connectReplica(interest);
        auto replica = std::move(*connection.replica);
        pumpBaseline(replica);
        fixture.host->advance(17ms);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
        const auto previousRevision = replica.revision();
        const auto previousTick = replica.tick();
        WorldChangeBatch unchangedTick{
            .content = fixture.host->content().identity(),
            .world = 0,
            .zone = "base:default",
            .baseRevision = previousRevision,
            .revision = previousRevision + 1,
            .tick = previousTick,
            .changes = {{address, {"rigel:water", 0}, 0}},
        };
        CHECK_EQ(
            replica.accept(std::make_shared<const PublicationMessage>(
                unchangedTick)),
            ReplicaAcceptStatus::Queued);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
        CHECK_EQ(replica.revision(), previousRevision);
        CHECK_EQ(replica.tick(), previousTick);
        CHECK_EQ(replica.read(address).status, ExactReadStatus::Unavailable);
    }
}

TEST_CASE(LoopbackReplica_validates_old_batch_structure_before_duplicate_detection) {
    for (int malformed = 0; malformed < 3; ++malformed) {
        HostFixture fixture;
        const CellAddress address{5, 0, 5};
        auto connection = fixture.host->connectReplica({address, address});
        auto replica = std::move(*connection.replica);
        pumpBaseline(replica);
        fixture.host->advance(17ms);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
        WorldChangeBatch old{
            .content = fixture.host->content().identity(),
            .world = 0,
            .zone = "base:default",
            .baseRevision = replica.revision() - 1,
            .revision = replica.revision(),
            .tick = replica.tick(),
            .changes = {{address, fixture.host->read(address).state}},
        };
        if (malformed == 0) old.baseRevision = old.revision + 1;
        if (malformed == 1) {
            old.changes.front().state = {"base:air", 1};
            old.changes.front().lightLevel = 7;
        }
        if (malformed == 2) old.changes.push_back(old.changes.front());
        CHECK_EQ(replica.accept(std::make_shared<const PublicationMessage>(old)),
                 ReplicaAcceptStatus::Queued);
        CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
    }
}

TEST_CASE(LoopbackReplica_rejects_unrepresentable_air_state) {
    HostFixture fixture;
    auto connection = fixture.host->connectReplica({{5, 0, 5}, {5, 0, 5}});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);

    WorldChangeBatch invalid{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
        .changes = {{{5, 0, 5}, {"base:air", 1}, 7}},
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(invalid)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
    CHECK_EQ(replica.read({5, 0, 5}).status, ExactReadStatus::Unavailable);
}

TEST_CASE(LoopbackReplica_rejects_oversized_string_payloads_by_bytes) {
    SimulationHostConfig config;
    config.maxReplicaBytes = 64 * 1024;
    HostFixture fixture(config);
    auto connection = fixture.host->connectReplica({{3, -1, 3}, {7, 3, 7}});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);

    WorldChangeBatch oversizedZone{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = std::string(128 * 1024, 'z'),
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(oversizedZone)),
        ReplicaAcceptStatus::RejectedOversized);
    CHECK(replica.needsResnapshot());

    CHECK_EQ(fixture.host->resnapshot(replica), ReplicaConnectStatus::Connected);
    pumpBaseline(replica);
    WorldChangeBatch oversizedKey{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
        .changes = {{{5, 0, 5}, {std::string(128 * 1024, 'b'), 0}, 0}},
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(oversizedKey)),
        ReplicaAcceptStatus::RejectedOversized);
    CHECK(replica.needsResnapshot());
}

TEST_CASE(LoopbackReplica_resnapshot_carries_complete_entity_presentation) {
    HostFixture fixture;
    auto entity = std::make_unique<Entity::Entity>();
    entity->setPosition({4.5f, 3.0f, 4.5f});
    const Entity::EntityId entityId =
        fixture.host->spawnEntity(std::move(entity));
    CHECK(!entityId.isNull());

    auto connection = fixture.host->connectReplica({{3, -1, 3}, {7, 3, 7}});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    const auto* initial = replica.appliedPublication();
    CHECK(initial != nullptr);
    const auto& initialEntities = std::get<WorldBaseline>(*initial).entities;
    CHECK_EQ(initialEntities.size(), size_t{2});
    CHECK(std::any_of(
        initialEntities.begin(), initialEntities.end(),
        [&](const PublishedEntity& value) {
            return value.state.id == entityId;
        }));

    WorldChangeBatch wrongBase{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = 20,
        .revision = 21,
        .tick = 1,
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(wrongBase)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);
    CHECK_EQ(
        fixture.host->resnapshot(replica),
        ReplicaConnectStatus::Connected);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    const auto* restored = replica.appliedPublication();
    CHECK(restored != nullptr);
    const auto& restoredEntities = std::get<WorldBaseline>(*restored).entities;
    CHECK_EQ(restoredEntities.size(), size_t{2});
    CHECK(std::any_of(
        restoredEntities.begin(), restoredEntities.end(),
        [&](const PublishedEntity& value) {
            return value.state.id == entityId;
        }));
}

TEST_CASE(LoopbackReplica_copies_and_validates_entity_presentation_payloads) {
    HostFixture fixture;
    auto connection = fixture.host->connectReplica({{3, -1, 3}, {7, 3, 7}});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);

    WorldChangeBatch batch{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
    };
    batch.entities.push_back({
        fixture.host->world().entities().get(fixture.actor)->simulationState()});
    const glm::vec3 admittedPosition = batch.entities.front().state.position;
    auto aliased = std::make_shared<PublicationMessage>(std::move(batch));
    CHECK_EQ(replica.accept(aliased), ReplicaAcceptStatus::Queued);
    std::get<WorldChangeBatch>(*aliased).entities.front().state.position.x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    const auto* applied = replica.appliedPublication();
    CHECK(applied != nullptr);
    CHECK_EQ(
        std::get<WorldChangeBatch>(*applied).entities.front().state.position,
        admittedPosition);

    WorldChangeBatch unsupported{
        .content = fixture.host->content().identity(),
        .world = 0,
        .zone = "base:default",
        .baseRevision = replica.revision(),
        .revision = replica.revision() + 1,
        .tick = replica.tick() + 1,
    };
    auto unsupportedState =
        fixture.host->world().entities().get(fixture.actor)->simulationState();
    unsupportedState.modelIdentifier = "entity_models/not_in_manifest";
    unsupported.entities.push_back({std::move(unsupportedState)});
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(
            std::move(unsupported))),
        ReplicaAcceptStatus::RejectedOversized);
    CHECK(replica.needsResnapshot());
}

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
TEST_CASE(SimulationHost_preflights_aggregate_dictionary_storage_without_replicas) {
    Voxel::WorldResources resources;
    std::string material;
    for (int index = 0; index < 16; ++index) {
        const auto key = "rigel:" + std::to_string(index) + std::string(1024, 'k');
        Voxel::BlockType block;
        block.identifier = key;
        resources.registry().registerBlock(key, std::move(block));
        material = key;
    }
    resources.registry().freeze();
    auto generator = std::make_shared<Voxel::WorldGenerator>(resources.registry(),
        flatDefinition(material, material, material), 17);
    SimulationHostConfig config;
    config.maxContentBytes = 4096;
    config.domain = {{0, 0, 0}, {0, 0, 0}};
    trackedAllocationBytes = 0;
    trackAllocations = true;
    bool rejected = false;
    try {
        SimulationHost host(resources, generator, config);
    } catch (const ContentManifestError& error) {
        rejected = std::string_view(error.what()).find("retained byte limit") !=
            std::string_view::npos;
    }
    trackAllocations = false;
    CHECK(rejected);
    // Only small host/error setup may allocate: the 16 KiB key table was never copied.
    CHECK(trackedAllocationBytes < config.maxContentBytes);
    config.maxContentBytes = 64 * 1024;
    SimulationHost accepted(resources, generator, config);
    const auto retained = accepted.content().retainedStorageBytes();
    CHECK(retained.has_value());
    CHECK(*retained <= config.maxContentBytes);
}

TEST_CASE(LoopbackReplica_preflights_public_copy_and_recovers_from_copy_failure) {
    for (const bool oversized : {false, true}) {
        SimulationHostConfig config;
        config.maxReplicaBytes = 64 * 1024;
        HostFixture fixture(config);
        const CellAddress address{5, 0, 5};
        auto connection = fixture.host->connectReplica({address, address});
        auto replica = std::move(*connection.replica);
        pumpBaseline(replica);
        auto sender = std::make_shared<PublicationMessage>(WorldChangeBatch{
            .content = fixture.host->content().identity(),
            .world = 0,
            .zone = "base:default",
            .baseRevision = replica.revision(),
            .revision = replica.revision() + 1,
            .tick = replica.tick() + 1,
        });
        if (oversized) {
            std::get<WorldChangeBatch>(*sender).zone.reserve(
                config.maxReplicaBytes * 2);
        }
        trackedAllocations = 0;
        trackAllocations = oversized;
        failureAllocationSize = 0;
        allocationsBeforeFailure = 0;
        failAllocation = !oversized;
        const auto status = replica.accept(sender);
        failAllocation = false;
        trackAllocations = false;
        CHECK_EQ(status, ReplicaAcceptStatus::RejectedOversized);
        if (oversized) CHECK_EQ(trackedAllocations, size_t{0});
        CHECK_EQ(sender.use_count(), 1L);
        CHECK(replica.needsResnapshot());
        CHECK_EQ(replica.queuedMessages(), size_t{0});
        CHECK_EQ(fixture.host->resnapshot(replica), ReplicaConnectStatus::Connected);
        pumpBaseline(replica);
        CHECK_EQ(replica.read(address).state, fixture.host->read(address).state);
    }
}

TEST_CASE(SimulationHost_healthy_replica_refresh_preserves_cut_and_pending_delivery) {
    HostFixture fixture;
    fixture.start();
    const auto address = fixture.surface();
    auto connection = fixture.host->connectReplica({address, address});
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);
    const auto before = replica.read(address).state;
    const auto tick = replica.tick();
    const auto revision = replica.revision();
    const auto command = fixture.removeCommand(1);
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    fixture.host->advance(17ms);
    CHECK_EQ(replica.queuedMessages(), size_t{1});

    failureAllocationSize = 0;
    allocationsBeforeFailure = 0;
    failAllocation = true;
    const auto status = fixture.host->resnapshot(replica);
    failAllocation = false;
    CHECK_EQ(status, ReplicaConnectStatus::NotNeeded);
    CHECK(!replica.needsResnapshot());
    CHECK_EQ(replica.read(address).status, ExactReadStatus::Known);
    CHECK_EQ(replica.read(address).state, before);
    CHECK_EQ(replica.tick(), tick);
    CHECK_EQ(replica.revision(), revision);
    CHECK_EQ(replica.queuedMessages(), size_t{1});
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::Applied);
    CHECK_EQ(replica.takeOutcome(), fixture.host->submit(command).outcome);
}

TEST_CASE(SimulationHost_preflights_long_baselines_and_resnapshot_retries) {
    auto makeHost = [](size_t byteLimit) {
        struct Fixture {
            std::unique_ptr<Voxel::WorldResources> resources =
                std::make_unique<Voxel::WorldResources>();
            std::shared_ptr<Voxel::WorldGenerator> generator;
            std::unique_ptr<SimulationHost> host;
            std::string blockKey;
            std::string zone;
        } fixture;
        fixture.blockKey = "rigel:" + std::string(2048, 'b');
        fixture.zone = "rigel:" + std::string(2048, 'z');
        Voxel::BlockType block;
        block.identifier = fixture.blockKey;
        fixture.resources->registry().registerBlock(
            fixture.blockKey, std::move(block));
        fixture.resources->registry().freeze();
        fixture.generator = std::make_shared<Voxel::WorldGenerator>(
            fixture.resources->registry(),
            flatDefinition(
                fixture.blockKey, fixture.blockKey, fixture.blockKey), 17);
        SimulationHostConfig config;
        config.zone = fixture.zone;
        config.domain = {{0, 0, 0}, {0, 0, 0}};
        config.preloadedChunks = {{0, 0, 0}};
        config.maxPreloadedChunks = 1;
        config.maxSnapshotCells = 1;
        config.maxReplicaQueue = 1;
        config.maxReplicas = 1;
        config.maxReplicaBytes = byteLimit;
        fixture.host = std::make_unique<SimulationHost>(
            *fixture.resources, fixture.generator, std::move(config));
        return fixture;
    };

    auto rejected = makeHost(1024);
    trackedAllocations = 0;
    trackedAllocationBytes = 0;
    trackAllocations = true;
    const auto rejectedConnection = rejected.host->connectReplica(
        {{0, 0, 0}, {0, 0, 0}});
    trackAllocations = false;
    CHECK_EQ(rejectedConnection.status, ReplicaConnectStatus::Capacity);
    CHECK_EQ(trackedAllocations, static_cast<size_t>(0));
    CHECK_EQ(trackedAllocationBytes, static_cast<size_t>(0));

    auto accepted = makeHost(16 * 1024);
    failureAllocationSize = 0;
    allocationsBeforeFailure = 0;
    failAllocation = true;
    CHECK_EQ(
        accepted.host->connectReplica({{0, 0, 0}, {0, 0, 0}}).status,
        ReplicaConnectStatus::Capacity);
    failAllocation = false;
    auto connection = accepted.host->connectReplica({{0, 0, 0}, {0, 0, 0}});
    CHECK_EQ(connection.status, ReplicaConnectStatus::Connected);
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);
    CHECK_EQ(replica.read({0, 0, 0}).state.blockKey, accepted.blockKey);

    WorldChangeBatch wrongBase{
        .content = accepted.host->content().identity(),
        .world = 0,
        .zone = accepted.zone,
        .baseRevision = 1,
        .revision = 2,
        .tick = 1,
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(wrongBase)),
        ReplicaAcceptStatus::Queued);
    CHECK_EQ(replica.pumpOne(), ReplicaPumpStatus::NeedsResnapshot);

    failureAllocationSize = 0;
    allocationsBeforeFailure = 0;
    failAllocation = true;
    CHECK_EQ(
        accepted.host->resnapshot(replica),
        ReplicaConnectStatus::Capacity);
    failAllocation = false;
    CHECK(replica.needsResnapshot());
    CHECK_EQ(replica.queuedMessages(), static_cast<size_t>(0));
    CHECK_EQ(replica.read({0, 0, 0}).status, ExactReadStatus::Unavailable);

    CHECK_EQ(
        accepted.host->resnapshot(replica),
        ReplicaConnectStatus::Connected);
    pumpBaseline(replica);
    CHECK_EQ(replica.read({0, 0, 0}).status, ExactReadStatus::Known);
}
#endif
