#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityTags.h"
#include "Rigel/Simulation/SimulationHost.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <chrono>
#include <memory>

namespace {

using namespace Rigel;
using namespace Rigel::Simulation;
using namespace std::chrono_literals;

Voxel::GeneratorDefinitionData flatDefinition() {
    auto data = Test::generatorDefinitionFixture(
        "rigel:stone", "rigel:grass", "rigel:water");
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
        glm::vec3 actorPosition = {12.0f, 4.0f, 12.0f}
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
        resources.registry().freeze();
        generator = std::make_shared<Voxel::WorldGenerator>(
            resources.registry(), flatDefinition(), 17);

        if (config.domain == CellBounds{{0, 0, 0}, {31, 31, 31}}) {
            config.domain = {{-4, -4, -4}, {20, 8, 20}};
            config.maxPreloadedChunks = 8;
            config.maxSnapshotCells = 25'000;
        }
        host = std::make_unique<SimulationHost>(resources, generator, config);
        auto entity = std::make_unique<Entity::Entity>("rigel:test_actor");
        entity->addTag(Entity::EntityTags::NoClip);
        entity->setPosition(actorPosition);
        entity->setVelocity({1.0f, 0.0f, 0.0f});
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
                .origin = {5.5f, 6.0f, 5.5f},
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
    CHECK_EQ(fixture.host->submit(command).status, SubmitStatus::Accepted);
    CHECK_EQ(fixture.host->advance(17ms).ticksRun, static_cast<size_t>(1));
    CHECK_EQ(
        fixture.host->submit(command).outcome->status,
        CommandOutcomeStatus::Unavailable);
    CHECK_EQ(
        fixture.host->read(available).state.blockKey,
        std::string("base:air"));
    CHECK_EQ(fixture.host->read(unavailable).status, ExactReadStatus::Unavailable);
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

TEST_CASE(SimulationHost_fixed_tick_is_render_pacing_independent_and_retains_debt) {
    auto run = [](int frames) {
        HostFixture fixture;
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

TEST_CASE(LoopbackReplica_rejects_incomplete_and_oversized_messages) {
    SimulationHostConfig config;
    config.maxChangesPerCommand = 2;
    HostFixture fixture(config);
    auto connection = fixture.host->connectReplica({{3, -1, 3}, {7, 3, 7}});
    auto replica = std::move(*connection.replica);
    pumpBaseline(replica);

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

    CHECK_EQ(
        fixture.host->connectReplica({{-5, 0, 0}, {1, 1, 1}}).status,
        ReplicaConnectStatus::InvalidInterest);
}
