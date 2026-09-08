#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "GraphicalAuthorityClient.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Entity/EntityTags.h"
#include "Rigel/Persistence/InMemoryStorage.h"
#include "Rigel/Simulation/SimulationCheckpoint.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <GLFW/glfw3.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
namespace {
bool failAllocation = false;
size_t allocationsBeforeFailure = 0;
size_t failureAllocationSize = 0;
}

void* operator new(std::size_t bytes) {
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
using namespace std::chrono_literals;

class SwitchableEntityModelLoader final : public Asset::IAssetLoader {
public:
    explicit SwitchableEntityModelLoader(std::shared_ptr<bool> fail)
        : m_fail(std::move(fail)) {
    }

    std::string_view category() const override { return "entity_models"; }

    std::shared_ptr<Asset::AssetBase> load(
        const Asset::LoadContext& context
    ) override {
        if (*m_fail) {
            throw Asset::AssetLoadError(
                context.id, "injected presentation model failure");
        }
        return m_loader.load(context);
    }

private:
    std::shared_ptr<bool> m_fail;
    Entity::EntityModelLoader m_loader;
};

Voxel::GeneratorDefinitionData flatDefinition() {
    auto data = Test::generatorDefinitionFixture(
        "rigel:stone", "rigel:stone", "rigel:stone");
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

Voxel::BlockID addModelBlock(
    Voxel::WorldResources& resources,
    std::string identifier,
    Voxel::BlockModelBounds bounds
) {
    Voxel::BlockModelCuboid cuboid;
    cuboid.bounds = bounds;
    for (auto& face : cuboid.faces) {
        face = Voxel::BlockModelFace{.textureSlot = "invented"};
    }
    Voxel::BlockType type;
    type.identifier = identifier;
    type.model = Voxel::BlockModelInstance(
        std::make_shared<const Voxel::BlockModel>(
            identifier + "_model",
            std::vector<std::string>{"invented"},
            std::vector<Voxel::BlockModelCuboid>{cuboid}));
    return resources.registry().registerBlock(identifier, std::move(type));
}

struct GraphicalFixture {
    Voxel::WorldResources resources;
    std::shared_ptr<Voxel::WorldGenerator> generator;
    std::unique_ptr<Simulation::SimulationHost> host;
    Voxel::World replica;
    Asset::AssetManager assets;
    Entity::EntityId observer;
    Entity::EntityId modeledEntity;
    std::unique_ptr<detail::GraphicalAuthorityClient> client;
    Input::CameraState camera;
    Input::WindowState window;
    Input::InputState input;

    uint64_t setupCommand = 100;

    explicit GraphicalFixture(
        size_t maxSessionReceipts = 256,
        bool connectClient = true,
        std::string initialModel = "entity_models/demo_cube",
        std::shared_ptr<bool> modelLoadFailure = {})
        : replica(resources) {
        Voxel::BlockType stone;
        stone.identifier = "rigel:stone";
        resources.registry().registerBlock("rigel:stone", std::move(stone));
        addModelBlock(
            resources, "rigel:slab",
            {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}});
        addModelBlock(
            resources, "rigel:overhang",
            {{-0.25f, 0.0f, 0.0f}, {0.25f, 1.0f, 1.0f}});
        resources.registry().freeze();
        if (modelLoadFailure) {
            assets.registerLoader(
                "entity_models",
                std::make_unique<SwitchableEntityModelLoader>(
                    std::move(modelLoadFailure)));
        } else {
            assets.registerLoader(
                "entity_models", std::make_unique<Entity::EntityModelLoader>());
        }
        assets.registerLoader(
            "entity_anims",
            std::make_unique<Entity::EntityAnimationSetLoader>());
        assets.loadManifest("manifest.yaml");
        generator = std::make_shared<Voxel::WorldGenerator>(
            resources.registry(), flatDefinition(), 17);

        Simulation::SimulationHostConfig config;
        config.domain = {{-4, -4, -4}, {20, 8, 20}};
        config.maxPreloadedChunks = 8;
        config.maxSnapshotCells = 25'000;
        config.maxSessionReceipts = maxSessionReceipts;
        host = std::make_unique<Simulation::SimulationHost>(
            resources, generator, config);
        auto entity = std::make_unique<Entity::Entity>();
        entity->addTag(Entity::EntityTags::NoClip);
        entity->addTag(Entity::EntityTags::LocalObserver);
        camera.position = {5.5f, 6.0f, 5.5f};
        camera.forward = {0.0f, -1.0f, 0.0f};
        entity->setPosition(camera.position);
        observer = host->spawnEntity(std::move(entity));
        CHECK(!observer.isNull());
        auto modeled = std::make_unique<Entity::Entity>();
        modeled->setPosition({7.5f, 2.0f, 7.5f});
        if (!initialModel.empty()) {
            modeled->setModelIdentifier(std::move(initialModel));
        }
        modeledEntity = host->spawnEntity(std::move(modeled));
        CHECK(!modeledEntity.isNull());
        CHECK_EQ(host->startSession(
            1, observer, host->content().identity()),
            Simulation::SessionStartStatus::Started);

        replica.setGenerator(generator);
        if (connectClient) {
            connect();
        }
        window.cursorCaptured = true;
        auto bindings = std::make_shared<Input::InputBindings>();
        bindings->bind("remove_block", GLFW_KEY_R);
        bindings->bind("place_block", GLFW_KEY_P);
        input.setBindings(std::move(bindings));
        input.beginFrame();
    }

    void connect() {
        const Simulation::CellBounds interest{
            {-4, -4, -4}, {20, 8, 20}};
        client = std::make_unique<detail::GraphicalAuthorityClient>(
            *host, replica, assets, interest,
            observer, 1, "rigel:stone");
    }

    Entity::EntityId spawnModeled(std::string identifier) {
        auto entity = std::make_unique<Entity::Entity>();
        entity->setPosition({8.5f, 2.0f, 8.5f});
        entity->setModelIdentifier(std::move(identifier));
        return host->spawnEntity(std::move(entity));
    }

    std::optional<Voxel::BlockTarget> target() const {
        return Voxel::raycastBlock(
            replica, camera.position, camera.forward, 8.0f);
    }

    bool capture(const Voxel::BlockTarget* targetValue) {
        return Input::handleBlockEdits(
            input, window, targetValue,
            Input::GameplayMutationMode::ReadWrite,
            [&](Input::GameplayBlockEditAction action,
                const Voxel::BlockTarget& selected) {
                return client->submit(action, selected, camera).accepted();
            });
    }

    void install(std::vector<std::pair<Simulation::CellAddress, std::string>> cells) {
        Simulation::EditCommand command{
            .session = 1,
            .command = setupCommand++,
            .actor = observer,
            .world = host->world().id(),
            .zone = "base:default",
            .content = host->content().identity(),
            .action = Simulation::EditAction::Atomic,
        };
        for (auto& [address, replacement] : cells) {
            command.mutations.push_back({
                .address = address,
                .expected = host->read(address).state,
                .replacement = {std::move(replacement), 0, 0},
            });
        }
        CHECK_EQ(
            host->submit(std::move(command), host->authorityEditCapability())
                .status,
            Simulation::SubmitStatus::Accepted);
        client->advance(17ms);
        CHECK_EQ(client->outcomes().back().status,
                 Simulation::CommandOutcomeStatus::Applied);
    }
};

} // namespace

TEST_CASE(GraphicalAuthorityClient_InputQueuesTickAndAppliesPublication) {
    GraphicalFixture fixture;
    CHECK_NE(&fixture.host->world(), &fixture.replica);
    CHECK(fixture.replica.entities().get(fixture.modeledEntity));
    CHECK(fixture.replica.entities().get(fixture.modeledEntity)->model());
    const auto selected = fixture.target();
    CHECK(selected.has_value());
    const Simulation::CellAddress address{
        selected->block.x, selected->block.y, selected->block.z};

    fixture.input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    fixture.input.beginFrame();
    CHECK(fixture.capture(&*selected));
    CHECK_NE(
        fixture.host->read(address).state.blockKey,
        std::string("base:air"));
    CHECK(!fixture.replica.getBlock(
        address.x, address.y, address.z).isAir());

    const auto advance = fixture.client->advance(17ms);
    CHECK_EQ(advance.ticksRun, static_cast<size_t>(1));
    CHECK_EQ(
        fixture.host->read(address).state.blockKey,
        std::string("base:air"));
    CHECK(fixture.replica.getBlock(address.x, address.y, address.z).isAir());
    CHECK_EQ(fixture.client->outcomes().size(), static_cast<size_t>(1));
    CHECK_EQ(
        fixture.client->outcomes().front().status,
        Simulation::CommandOutcomeStatus::Applied);
    CHECK(!fixture.client->changedChunks().empty());

    fixture.input.beginFrame();
    const auto heldTarget = fixture.target();
    CHECK(heldTarget.has_value());
    CHECK(!fixture.capture(&*heldTarget));
    fixture.client->advance(17ms);
    CHECK(fixture.client->outcomes().empty());

    fixture.input.handleKeyEvent(GLFW_KEY_R, GLFW_RELEASE);
    fixture.input.handleKeyEvent(GLFW_KEY_P, GLFW_PRESS);
    fixture.input.beginFrame();
    const auto placementTarget = fixture.target();
    CHECK(placementTarget.has_value());
    const glm::ivec3 destination =
        placementTarget->block + placementTarget->normal;
    CHECK(fixture.capture(&*placementTarget));
    fixture.client->advance(17ms);
    CHECK_EQ(
        fixture.host->read({destination.x, destination.y, destination.z})
            .state.blockKey,
        std::string("rigel:stone"));
    CHECK_EQ(
        fixture.replica.getBlock(destination.x, destination.y, destination.z).id,
        fixture.resources.registry().findByIdentifier("rigel:stone").value());

    const auto recording = fixture.host->recording();
    CHECK(recording.has_value());
    auto replayed = Simulation::SimulationHost::resimulate(
        fixture.resources, fixture.generator, *recording, {7ms, 10ms});
    CHECK_EQ(replayed.status, Simulation::ResimulationStatus::Complete);
    CHECK_EQ(replayed.stateHash, fixture.host->stateHash());

    CHECK(fixture.host->despawnEntity(fixture.modeledEntity));
    fixture.client->advance(17ms);
    CHECK(!fixture.replica.entities().get(fixture.modeledEntity));
}

#ifdef RIGEL_TEST_ALLOCATION_FAILURES
TEST_CASE(GraphicalAuthorityClient_BaselineAllocationFailureLeavesWorldEmpty) {
    GraphicalFixture fixture(256, false, "");
    failureAllocationSize = sizeof(std::array<
        Voxel::BlockState, Voxel::Chunk::SUBCHUNK_VOLUME>);
    allocationsBeforeFailure = 0;
    failAllocation = true;
    CHECK_THROWS(fixture.connect());
    failAllocation = false;
    failureAllocationSize = 0;

    CHECK_EQ(fixture.replica.chunkManager().loadedChunkCount(), size_t{0});
    CHECK_EQ(fixture.replica.entities().size(), size_t{0});

    fixture.connect();
    CHECK_EQ(fixture.client->revision(), fixture.host->revision());
    CHECK_EQ(fixture.replica.entities().size(),
             fixture.host->world().entities().size());
}

TEST_CASE(GraphicalAuthorityClient_ChangeAllocationFailureRetriesWholeCut) {
    GraphicalFixture fixture(256, true, "");
    const Simulation::Revision visibleRevision = fixture.client->revision();
    const auto selected = fixture.target();
    CHECK(selected.has_value());
    const Simulation::CellAddress removed{
        selected->block.x, selected->block.y, selected->block.z};
    CHECK(fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        *selected,
        fixture.camera).accepted());

    const std::array<Simulation::CellAddress, 2> placed{{
        {1, 7, 1}, {17, 7, 17},
    }};
    Simulation::EditCommand command{
        .session = 1,
        .command = fixture.setupCommand++,
        .actor = fixture.observer,
        .world = fixture.host->world().id(),
        .zone = "base:default",
        .content = fixture.host->content().identity(),
        .action = Simulation::EditAction::Atomic,
    };
    for (const auto address : placed) {
        command.mutations.push_back({
            .address = address,
            .expected = fixture.host->read(address).state,
            .replacement = {"rigel:stone", 0, 0},
        });
    }
    CHECK_EQ(
        fixture.host->submit(
            std::move(command), fixture.host->authorityEditCapability()).status,
        Simulation::SubmitStatus::Accepted);
    const Entity::EntityId added =
        fixture.spawnModeled("entity_models/demo_cube");
    CHECK(!added.isNull());

    const auto removedBefore = fixture.replica.getBlock(
        removed.x, removed.y, removed.z);
    const glm::vec3 entityBefore = fixture.replica.entities()
        .get(fixture.modeledEntity)->position();
    failureAllocationSize = sizeof(std::array<
        Voxel::BlockState, Voxel::Chunk::SUBCHUNK_VOLUME>);
    allocationsBeforeFailure = 0;
    failAllocation = true;
    CHECK_NO_THROW(fixture.client->advance(34ms));
    failAllocation = false;
    failureAllocationSize = 0;

    CHECK_EQ(fixture.client->revision(), visibleRevision);
    CHECK(fixture.client->projectionRetryPending());
    CHECK_EQ(fixture.replica.getBlock(
                 removed.x, removed.y, removed.z), removedBefore);
    for (const auto address : placed) {
        CHECK(fixture.replica.getBlock(
            address.x, address.y, address.z).isAir());
        CHECK_EQ(fixture.host->read(address).state.blockKey,
                 std::string("rigel:stone"));
    }
    CHECK(!fixture.replica.entities().get(added));
    CHECK_EQ(fixture.replica.entities().get(
                 fixture.modeledEntity)->position(), entityBefore);
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{1});
    CHECK(fixture.client->outcomes().empty());

    CHECK_NO_THROW(fixture.client->advance(0ns));
    CHECK(!fixture.client->projectionRetryPending());
    CHECK_EQ(fixture.client->revision(), fixture.host->revision());
    CHECK(fixture.replica.getBlock(
        removed.x, removed.y, removed.z).isAir());
    for (const auto address : placed) {
        CHECK(!fixture.replica.getBlock(
            address.x, address.y, address.z).isAir());
    }
    const Entity::Entity* projected = fixture.replica.entities().get(added);
    CHECK(projected);
    CHECK(projected->model());
    CHECK_EQ(fixture.replica.entities().get(
                 fixture.modeledEntity)->position(),
             fixture.host->world().entities().get(
                 fixture.modeledEntity)->position());
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{0});
    CHECK_EQ(fixture.client->outcomes().size(), size_t{2});

    fixture.client->advance(0ns);
    CHECK(fixture.client->outcomes().empty());
    const auto nextTarget = fixture.target();
    CHECK(nextTarget.has_value());
    CHECK(fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        *nextTarget,
        fixture.camera).accepted());
    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{0});
    CHECK_EQ(fixture.client->outcomes().size(), size_t{1});
}
#endif

TEST_CASE(GraphicalAuthorityClient_BaselineModelFailureLeavesWorldEmpty) {
    auto failModel = std::make_shared<bool>(true);
    GraphicalFixture failingLoader(
        256, false, "entity_models/demo_cube", failModel);
    CHECK_THROWS(failingLoader.connect());
    CHECK_EQ(failingLoader.replica.chunkManager().loadedChunkCount(), size_t{0});
    CHECK_EQ(failingLoader.replica.entities().size(), size_t{0});

    *failModel = false;
    failingLoader.connect();
    CHECK(failingLoader.replica.entities().get(
        failingLoader.modeledEntity)->model());

    GraphicalFixture missing(
        256, false, "entity_models/model_drone_interceptor");
    if (!missing.assets.exists("entity_models/model_drone_interceptor")) {
        CHECK_THROWS(missing.connect());
        CHECK_EQ(missing.replica.chunkManager().loadedChunkCount(), size_t{0});
        CHECK_EQ(missing.replica.entities().size(), size_t{0});
    }
}

TEST_CASE(GraphicalAuthorityClient_ChangeModelFailureRetainsRetry) {
    auto failModel = std::make_shared<bool>(false);
    GraphicalFixture fixture(256, true, "", failModel);
    const Simulation::Revision visibleRevision = fixture.client->revision();
    const size_t visibleEntities = fixture.replica.entities().size();
    const Entity::EntityId added =
        fixture.spawnModeled("entity_models/demo_cube");
    CHECK(!added.isNull());

    *failModel = true;
    CHECK_NO_THROW(fixture.client->advance(17ms));
    CHECK_EQ(fixture.client->revision(), visibleRevision);
    CHECK(fixture.client->projectionRetryPending());
    CHECK_EQ(fixture.replica.entities().size(), visibleEntities);
    CHECK(!fixture.replica.entities().get(added));
    CHECK_NO_THROW(fixture.client->advance(17ms));
    CHECK_EQ(fixture.host->revision(), visibleRevision + 1);

    *failModel = false;
    CHECK_NO_THROW(fixture.client->advance(0ns));
    CHECK(!fixture.client->projectionRetryPending());
    CHECK_EQ(fixture.client->revision(), fixture.host->revision());
    CHECK_EQ(fixture.client->revision(), visibleRevision + 2);
    CHECK(fixture.replica.entities().get(added));
    CHECK(fixture.replica.entities().get(added)->model());

    GraphicalFixture missing(256, true, "");
    if (!missing.assets.exists("entity_models/model_drone_interceptor")) {
        const Simulation::Revision missingRevision = missing.client->revision();
        const Entity::EntityId unavailable =
            missing.spawnModeled("entity_models/model_drone_interceptor");
        CHECK(!unavailable.isNull());
        CHECK_NO_THROW(missing.client->advance(17ms));
        CHECK_EQ(missing.client->revision(), missingRevision);
        CHECK(missing.client->projectionRetryPending());
        CHECK(!missing.replica.entities().get(unavailable));
        CHECK_NO_THROW(missing.client->advance(17ms));
        CHECK_EQ(missing.host->revision(), missingRevision + 1);
        CHECK_EQ(missing.client->revision(), missingRevision);
    }
}

TEST_CASE(GraphicalAuthorityClient_RetryResnapshotsAfterProducerPressure) {
    auto failModel = std::make_shared<bool>(false);
    GraphicalFixture fixture(256, true, "", failModel);
    const auto selected = fixture.target();
    CHECK(selected.has_value());
    const Simulation::Revision visibleRevision = fixture.client->revision();
    CHECK(fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        *selected,
        fixture.camera).accepted());
    const Entity::EntityId added =
        fixture.spawnModeled("entity_models/demo_cube");
    CHECK(!added.isNull());

    *failModel = true;
    fixture.client->advance(17ms);
    CHECK(fixture.client->projectionRetryPending());
    CHECK_EQ(fixture.client->revision(), visibleRevision);
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{1});

    for (int pass = 0; pass < 5; ++pass) {
        fixture.host->advance(250ms);
    }
    *failModel = false;
    fixture.client->advance(0ns);
    CHECK(!fixture.client->projectionRetryPending());
    CHECK_EQ(fixture.client->revision(), fixture.host->revision());
    CHECK(fixture.replica.entities().get(added));
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{0});
    CHECK_EQ(fixture.client->outcomes().size(), size_t{1});
    CHECK_EQ(fixture.client->outcomes().front().status,
             Simulation::CommandOutcomeStatus::Applied);
    fixture.client->advance(0ns);
    CHECK(fixture.client->outcomes().empty());
}

TEST_CASE(GraphicalAuthorityClient_ReattachesToRecoveredPendingSession) {
    GraphicalFixture fixture;
    const auto selected = fixture.target();
    CHECK(selected.has_value());
    const Simulation::CellAddress address{
        selected->block.x, selected->block.y, selected->block.z};
    CHECK(fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        *selected,
        fixture.camera).accepted());
    CHECK_NE(fixture.host->read(address).state.blockKey,
             std::string("base:air"));

    auto storage =
        std::make_shared<Persistence::InMemoryStorageBackend>();
    {
        Simulation::SimulationCheckpointManager manager(
            storage, "/graphical-pending");
        CHECK_EQ(manager.request(*fixture.host),
                 Simulation::CheckpointRequestStatus::Started);
        while (manager.writeInFlight()) std::this_thread::yield();
        const auto outcome = manager.poll();
        CHECK(outcome.has_value());
        CHECK_EQ(outcome->status,
                 Simulation::CheckpointWriteStatus::Durable);
    }

    fixture.client.reset();
    fixture.host.reset();
    Simulation::SimulationCheckpointManager reopened(
        storage, "/graphical-pending");
    auto recovery = reopened.recover(fixture.resources, fixture.generator);
    CHECK_EQ(recovery.status,
             Simulation::CheckpointRecoveryStatus::Recovered);
    CHECK(recovery.host != nullptr);
    const auto session = recovery.host->activeSession();
    CHECK(session.has_value());
    CHECK_EQ(session->session, Simulation::SessionId{1});
    CHECK_EQ(session->actor, fixture.observer);
    CHECK_EQ(session->nextCommand, Simulation::CommandId{2});
    CHECK_EQ(session->pendingCommands,
             std::vector<Simulation::CommandId>{1});
    CHECK_EQ(recovery.host->startSession(
        recovery.host->nextSessionId(),
        fixture.observer,
        recovery.host->content().identity()),
        Simulation::SessionStartStatus::Busy);

    Voxel::World resumedReplica(fixture.resources);
    resumedReplica.setGenerator(fixture.generator);
    detail::GraphicalAuthorityClient resumed(
        *recovery.host,
        resumedReplica,
        fixture.assets,
        {{-4, -4, -4}, {20, 8, 20}},
        session->actor,
        session->session,
        "rigel:stone",
        session->nextCommand,
        session->pendingCommands);
    CHECK_EQ(resumed.pendingSubmissionCount(), size_t{1});

    const auto advanced = resumed.advance(17ms);
    CHECK_EQ(advanced.ticksRun, size_t{1});
    CHECK_EQ(resumed.outcomes().size(), size_t{1});
    CHECK_EQ(resumed.outcomes().front().status,
             Simulation::CommandOutcomeStatus::Applied);
    CHECK_EQ(resumed.pendingSubmissionCount(), size_t{0});
    CHECK_EQ(recovery.host->read(address).state.blockKey,
             std::string("base:air"));
    CHECK(resumedReplica.getBlock(address.x, address.y, address.z).isAir());

    resumed.advance(17ms);
    CHECK(resumed.outcomes().empty());
}

TEST_CASE(GraphicalAuthorityClient_NoTargetAndReadOnlyDoNotSubmit) {
    GraphicalFixture fixture;
    fixture.input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    fixture.input.beginFrame();
    CHECK(!fixture.capture(nullptr));

    const auto selected = fixture.target();
    CHECK(selected.has_value());
    CHECK(!Input::handleBlockEdits(
        fixture.input, fixture.window, &*selected,
        Input::GameplayMutationMode::ReadOnly,
        [&](Input::GameplayBlockEditAction action,
            const Voxel::BlockTarget& target) {
            return fixture.client->submit(
                action, target, fixture.camera).accepted();
        }));
    fixture.client->advance(17ms);
    CHECK(fixture.client->outcomes().empty());
}

TEST_CASE(GraphicalAuthorityClient_SessionCapacityRotatesAfterTerminalOutcome) {
    GraphicalFixture fixture(1);
    const auto selected = fixture.target();
    CHECK(selected.has_value());

    auto first = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove, *selected, fixture.camera);
    CHECK(first.accepted());
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{1});

    const auto deferred = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove, *selected, fixture.camera);
    CHECK_EQ(
        deferred.status,
        detail::GraphicalEditSubmitStatus::SessionReplacementDeferred);
    CHECK_EQ(deferred.hostStatus, Simulation::SubmitStatus::ReceiptCapacity);
    CHECK_EQ(fixture.client->session(), Simulation::SessionId{1});

    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->pendingSubmissionCount(), size_t{0});
    const auto replacement = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove, *selected, fixture.camera);
    CHECK_EQ(
        replacement.status,
        detail::GraphicalEditSubmitStatus::AcceptedAfterSessionReplacement);
    CHECK_EQ(fixture.client->session(), Simulation::SessionId{2});
    fixture.client->advance(17ms);

    const auto& stats = fixture.client->submissionStats();
    CHECK_EQ(stats.accepted, uint64_t{2});
    CHECK_EQ(stats.rejected, uint64_t{1});
    CHECK_EQ(stats.sessionReplacements, uint64_t{1});

    Input::CameraState invalidCamera = fixture.camera;
    invalidCamera.position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(
        fixture.client->submit(
            Input::GameplayBlockEditAction::Remove,
            *selected,
            invalidCamera).status,
        detail::GraphicalEditSubmitStatus::ObserverRejected);
    CHECK_EQ(fixture.client->submissionStats().rejected, uint64_t{2});
}

TEST_CASE(GraphicalAuthorityClient_AuthorityValidatesPartialShapeFaces) {
    GraphicalFixture fixture;
    fixture.install({
        {{0, 2, 0}, "rigel:stone"},
        {{0, 2, 1}, "rigel:slab"},
    });

    fixture.camera.position = {0.5f, 2.75f, 2.5f};
    fixture.camera.forward = {0.0f, 0.0f, -1.0f};
    auto beyondPartial = fixture.target();
    CHECK(beyondPartial.has_value());
    CHECK_EQ(beyondPartial->block, (glm::ivec3{0, 2, 0}));

    Voxel::BlockTarget wrongFace = *beyondPartial;
    wrongFace.face = Voxel::Direction::PosY;
    const auto rejected = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        wrongFace,
        fixture.camera);
    CHECK(rejected.accepted());
    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->outcomes().back().status,
             Simulation::CommandOutcomeStatus::TargetMismatch);
    CHECK(!fixture.replica.getBlock(0, 2, 0).isAir());

    const auto removal = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        *beyondPartial,
        fixture.camera);
    CHECK(removal.accepted());
    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->outcomes().back().status,
             Simulation::CommandOutcomeStatus::Applied);
    CHECK(fixture.replica.getBlock(0, 2, 0).isAir());
    CHECK_EQ(
        fixture.resources.registry()
            .getType(fixture.replica.getBlock(0, 2, 1).id).identifier,
        std::string("rigel:slab"));

    fixture.camera.position = {0.5f, 2.75f, 1.5f};
    fixture.camera.forward = {0.0f, -1.0f, 0.0f};
    const auto slabTop = fixture.target();
    CHECK(slabTop.has_value());
    CHECK_EQ(slabTop->normal, (glm::ivec3{0, 1, 0}));
    const auto placement = fixture.client->submit(
        Input::GameplayBlockEditAction::Place, *slabTop, fixture.camera);
    CHECK(placement.accepted());
    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->outcomes().back().status,
             Simulation::CommandOutcomeStatus::Applied);
    CHECK_EQ(
        fixture.resources.registry()
            .getType(fixture.replica.getBlock(0, 3, 1).id).identifier,
        std::string("rigel:stone"));
}

TEST_CASE(GraphicalAuthorityClient_AuthorityValidatesOverhangOwner) {
    GraphicalFixture fixture;
    fixture.install({{{1, 2, 0}, "rigel:overhang"}});
    fixture.camera.position = {0.5f, 2.5f, 0.5f};
    fixture.camera.forward = {1.0f, 0.0f, 0.0f};
    const auto selected = fixture.target();
    CHECK(selected.has_value());
    CHECK_EQ(selected->block, (glm::ivec3{1, 2, 0}));
    CHECK_NEAR(selected->position.x, 0.75f, 0.00001f);

    Voxel::BlockTarget wrongOwner = *selected;
    wrongOwner.block = {0, 2, 0};
    const auto rejected = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove,
        wrongOwner,
        fixture.camera);
    CHECK(rejected.accepted());
    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->outcomes().back().status,
             Simulation::CommandOutcomeStatus::TargetMismatch);
    CHECK(!fixture.replica.getBlock(1, 2, 0).isAir());

    const auto result = fixture.client->submit(
        Input::GameplayBlockEditAction::Remove, *selected, fixture.camera);
    CHECK(result.accepted());
    fixture.client->advance(17ms);
    CHECK_EQ(fixture.client->outcomes().back().status,
             Simulation::CommandOutcomeStatus::Applied);
    CHECK(fixture.replica.getBlock(1, 2, 0).isAir());
}
