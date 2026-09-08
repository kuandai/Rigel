#include "TestFramework.h"
#include "GeneratorDefinitionTestRegistry.h"

#include "GraphicalAuthorityClient.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Entity/EntityTags.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldResources.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace Rigel;
using namespace std::chrono_literals;

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

    explicit GraphicalFixture(size_t maxSessionReceipts = 256)
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
        assets.registerLoader(
            "entity_models", std::make_unique<Entity::EntityModelLoader>());
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
        modeled->setModelIdentifier("entity_models/demo_cube");
        modeledEntity = host->spawnEntity(std::move(modeled));
        CHECK(!modeledEntity.isNull());
        CHECK_EQ(host->startSession(
            1, observer, host->content().identity()),
            Simulation::SessionStartStatus::Started);

        replica.setGenerator(generator);
        client = std::make_unique<detail::GraphicalAuthorityClient>(
            *host, replica, assets, config.domain, observer, 1, "rigel:stone");
        window.cursorCaptured = true;
        auto bindings = std::make_shared<Input::InputBindings>();
        bindings->bind("remove_block", GLFW_KEY_R);
        bindings->bind("place_block", GLFW_KEY_P);
        input.setBindings(std::move(bindings));
        input.beginFrame();
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
