#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityTags.h"
#include "Rigel/Simulation/SimulationHost.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
namespace {
bool failAllocation = false;
size_t allocationsBeforeFailure = 0;
size_t failureAllocationSize = 0;
bool trackAllocations = false;
size_t trackedAllocations = 0;
size_t trackedAllocationBytes = 0;
}

void* operator new(std::size_t bytes) {
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
        glm::vec3 actorVelocity = {0.0f, 0.0f, 0.0f}
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
                .replacement = {"base:air", 0, 0},
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
            {available, {"base:air", 0, 0}, {"rigel:stone", 0, 0}},
            {unavailable, {"base:air", 0, 0}, {"rigel:stone", 0, 0}},
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
            .mutations = {{first, {"base:air", 0, 0},
                           {"rigel:stone", 0, 0}}},
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
                {first, {"rigel:stone", 0, 0}, {"base:air", 0, 0}},
                {second, {"base:air", 0, 0}, {"rigel:stone", 0, 0}},
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
            {first, {"base:air", 0, 0}, {"rigel:stone", 0, 0}},
            {second, {"base:air", 0, 0}, {"rigel:stone", 0, 0}},
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
        {"base:air", 0, 0},
        {"rigel:stone", 0, 0},
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
        {"base:air", 0, 0},
        {"rigel:stone", 0, 0},
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
    CHECK_EQ(air, (SemanticBlockState{"base:air", 0, 0}));

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
        .mutations = {{address, air, {"base:air", 1, 7}}},
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
    supported.mutations.front().replacement = {"rigel:stone", 1, 7};
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
            {first, {"base:air", 0, 0}, {"rigel:stone", 0, 0}},
            {second, {"rigel:stone", 0, 0}, {"rigel:stone", 0, 0}},
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
    const SemanticBlockState air{"base:air", 0, 0};
    CHECK_EQ(survivor->read(target).state, air);
    CHECK_EQ(survivor->takeOutcome(), authoritative);
    CHECK(!survivor->takeOutcome().has_value());
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
        duplicate.cells.front().state = {"rigel:water", 0, 0};
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
            .changes = {{address, {"rigel:water", 0, 0}}},
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
        if (malformed == 1) old.changes.front().state = {"base:air", 1, 7};
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
        .changes = {{{5, 0, 5}, {"base:air", 1, 7}}},
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
        .changes = {{{5, 0, 5}, {std::string(128 * 1024, 'b'), 0, 0}}},
    };
    CHECK_EQ(
        replica.accept(std::make_shared<const PublicationMessage>(oversizedKey)),
        ReplicaAcceptStatus::RejectedOversized);
    CHECK(replica.needsResnapshot());
}

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
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
