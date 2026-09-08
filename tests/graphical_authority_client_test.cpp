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
#include <memory>

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

    GraphicalFixture() : replica(resources) {
        Voxel::BlockType stone;
        stone.identifier = "rigel:stone";
        resources.registry().registerBlock("rigel:stone", std::move(stone));
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
                return client->submit(action, selected, camera);
            });
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
            return fixture.client->submit(action, target, fixture.camera);
        }));
    fixture.client->advance(17ms);
    CHECK(fixture.client->outcomes().empty());
}
