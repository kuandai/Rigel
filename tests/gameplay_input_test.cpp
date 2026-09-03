#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"
#include "Rigel/input/GameplayInput.h"
#include "Rigel/Asset/AssetManager.h"

#include <GLFW/glfw3.h>

#include <memory>
#include <string>
#include <vector>

using namespace Rigel;

namespace {

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

Input::InputState pressedEditInput(std::string_view action, int key) {
    auto bindings = std::make_shared<Input::InputBindings>();
    bindings->bind(std::string(action), key);
    Input::InputState input;
    input.setBindings(std::move(bindings));
    input.beginFrame();
    input.handleKeyEvent(key, GLFW_PRESS);
    input.beginFrame();
    return input;
}

const Voxel::BlockTarget* targetPointer(
    const std::optional<Voxel::BlockTarget>& target
) {
    return target ? &*target : nullptr;
}

} // namespace

TEST_CASE(GameplayInput_CursorMathUsesEffectiveSensitivityAndInvertY) {
    Input::WindowState window;
    Input::CameraState camera;
    camera.yaw = 10.0f;
    camera.pitch = 5.0f;
    Preferences::InputPreferences preferences;
    preferences.mouseSensitivity = 0.5;
    Input::InputCallbackContext context;
    context.window = &window;
    context.camera = &camera;
    context.effectiveInputPreferences = &preferences;

    Input::applyCursorPosition(context, 100.0, 200.0);
    CHECK_EQ(camera.yaw, 10.0f);
    CHECK_EQ(camera.pitch, 5.0f);

    Input::applyCursorPosition(context, 104.0, 206.0);
    CHECK_EQ(camera.yaw, 12.0f);
    CHECK_EQ(camera.pitch, 2.0f);

    preferences.mouseSensitivity = 0.25;
    preferences.invertY = true;
    Input::applyCursorPosition(context, 108.0, 210.0);
    CHECK_EQ(camera.yaw, 13.0f);
    CHECK_EQ(camera.pitch, 3.0f);

    window.cursorCaptured = false;
    preferences.mouseSensitivity = 1.0;
    Input::applyCursorPosition(context, 200.0, 300.0);
    CHECK_EQ(camera.yaw, 13.0f);
    CHECK_EQ(camera.pitch, 3.0f);
}

TEST_CASE(GameplayInput_BlockEditsUseSemanticActions) {
    Voxel::WorldResources resources;
    Voxel::BlockType solid;
    solid.identifier = "rigel:input_test_solid";
    solid.isOpaque = true;
    const Voxel::BlockID solidId =
        resources.registry().registerBlock(solid.identifier, solid);
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);

    Input::CameraState camera;
    camera.position = {0.5f, 0.5f, 2.5f};
    camera.forward = {0.0f, 0.0f, -1.0f};
    Input::WindowState window;
    window.cursorCaptured = true;

    auto bindings = std::make_shared<Input::InputBindings>();
    bindings->bind("remove_block", GLFW_KEY_R);
    bindings->bind("place_block", GLFW_KEY_P);
    Input::InputState input;
    input.setBindings(bindings);
    input.beginFrame();

    world.setBlock(0, 0, 0, Voxel::BlockState{solidId});
    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    input.beginFrame();
    auto target = Voxel::raycastBlock(
        world, camera.position, camera.forward, 8.0f);
    Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, solidId,
        Input::GameplayMutationMode::ReadWrite);
    CHECK_EQ(world.getBlock(0, 0, 0).id, solidId);

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
    input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    input.beginFrame();
    target = Voxel::raycastBlock(
        world, camera.position, camera.forward, 8.0f);
    Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, solidId,
        Input::GameplayMutationMode::ReadWrite);
    CHECK(world.getBlock(0, 0, 0).isAir());

    input.handleKeyEvent(GLFW_KEY_R, GLFW_RELEASE);
    input.beginFrame();
    world.setBlock(0, 0, 0, Voxel::BlockState{solidId});
    input.handleKeyEvent(GLFW_KEY_P, GLFW_PRESS);
    input.beginFrame();
    target = Voxel::raycastBlock(
        world, camera.position, camera.forward, 8.0f);
    Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, solidId,
        Input::GameplayMutationMode::ReadWrite);
    CHECK_EQ(world.getBlock(0, 0, 1).id, solidId);
}

TEST_CASE(GameplayInput_ReadOnlyModeSuppressesWorldMutationsOnly) {
    Voxel::WorldResources resources;
    Voxel::BlockType solid;
    const std::string identifier = "invented:read_only_target";
    solid.identifier = identifier;
    const Voxel::BlockID solidId =
        resources.registry().registerBlock(identifier, std::move(solid));
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    Asset::AssetManager assets;

    Input::CameraState camera;
    camera.position = {0.5f, 0.5f, 2.5f};
    camera.forward = {0.0f, 0.0f, -1.0f};
    Input::WindowState window;
    window.cursorCaptured = true;

    auto bindings = std::make_shared<Input::InputBindings>();
    bindings->bind("move_forward", GLFW_KEY_W);
    bindings->bind("remove_block", GLFW_KEY_R);
    bindings->bind("place_block", GLFW_KEY_P);
    bindings->bind("demo_spawn_entity", GLFW_KEY_F2);
    Input::InputState input;
    input.setBindings(bindings);
    input.beginFrame();
    input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_P, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_F2, GLFW_PRESS);
    input.beginFrame();

    world.setBlock(0, 0, 0, Voxel::BlockState{solidId});
    const glm::vec3 initialCameraPosition = camera.position;
    Input::updateCamera(input, camera, 0.25f);
    Input::handleDemoSpawn(
        input,
        assets,
        world,
        camera,
        Input::GameplayMutationMode::ReadOnly);
    Input::handleBlockEdits(
        input,
        window,
        nullptr,
        world,
        view,
        solidId,
        Input::GameplayMutationMode::ReadOnly);

    CHECK_NE(camera.position, initialCameraPosition);
    CHECK_EQ(world.getBlock(0, 0, 0).id, solidId);
    CHECK(world.getBlock(0, 0, 1).isAir());
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
}

TEST_CASE(GameplayInput_RemovalTargetsBeyondPartialModelEmptySpace) {
    Voxel::WorldResources resources;
    const Voxel::BlockID slab = addModelBlock(
        resources,
        "invented:removal_slab",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}});
    const Voxel::BlockID fullCube = addModelBlock(
        resources,
        "invented:removal_background",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    world.setBlock(0, 0, 1, Voxel::BlockState{slab});
    world.setBlock(0, 0, 0, Voxel::BlockState{fullCube});

    const auto target = Voxel::raycastBlock(
        world, {0.5f, 0.75f, 2.5f}, {0.0f, 0.0f, -1.0f}, 8.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{0, 0, 0}));
    Input::InputState input = pressedEditInput("remove_block", GLFW_KEY_R);
    Input::WindowState window;

    CHECK(Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, fullCube,
        Input::GameplayMutationMode::ReadWrite));
    CHECK(world.getBlock(0, 0, 0).isAir());
    CHECK_EQ(world.getBlock(0, 0, 1).id, slab);
}

TEST_CASE(GameplayInput_PlacementUsesPartialModelFaceNormal) {
    Voxel::WorldResources resources;
    const Voxel::BlockID slab = addModelBlock(
        resources,
        "invented:placement_slab",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}});
    const Voxel::BlockID placed = addModelBlock(
        resources,
        "invented:placed_cube",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    world.setBlock(0, 0, 0, Voxel::BlockState{slab});

    const auto target = Voxel::raycastBlock(
        world, {0.5f, 0.75f, 0.5f}, {0.0f, -1.0f, 0.0f}, 8.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->normal, (glm::ivec3{0, 1, 0}));
    CHECK_NEAR(target->position.y, 0.5f, 0.00001f);
    Input::InputState input = pressedEditInput("place_block", GLFW_KEY_P);
    Input::WindowState window;

    CHECK(Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, placed,
        Input::GameplayMutationMode::ReadWrite));
    CHECK_EQ(world.getBlock(0, 1, 0).id, placed);
    CHECK_EQ(world.getBlock(0, 0, 0).id, slab);
}

TEST_CASE(GameplayInput_RemovalUsesOverhangingModelOwner) {
    Voxel::WorldResources resources;
    const Voxel::BlockID overhang = addModelBlock(
        resources,
        "invented:owner_overhang",
        {{-0.25f, 0.0f, 0.0f}, {0.25f, 1.0f, 1.0f}});
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    world.setBlock(1, 0, 0, Voxel::BlockState{overhang});

    const auto target = Voxel::raycastBlock(
        world, {0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 8.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{1, 0, 0}));
    CHECK_NEAR(target->position.x, 0.75f, 0.00001f);
    Input::InputState input = pressedEditInput("remove_block", GLFW_KEY_R);
    Input::WindowState window;

    CHECK(Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, overhang,
        Input::GameplayMutationMode::ReadWrite));
    CHECK(world.getBlock(1, 0, 0).isAir());
}

TEST_CASE(GameplayInput_RemovalInvalidatesTargetBeforeSimultaneousPlacement) {
    Voxel::WorldResources resources;
    const Voxel::BlockID targetBlock = addModelBlock(
        resources,
        "invented:simultaneous_target",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
    const Voxel::BlockID placedBlock = addModelBlock(
        resources,
        "invented:simultaneous_placed",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    world.setBlock(0, 0, 0, Voxel::BlockState{targetBlock});

    const auto target = Voxel::raycastBlock(
        world, {0.5f, 0.5f, 2.5f}, {0.0f, 0.0f, -1.0f}, 8.0f);
    CHECK(target.has_value());
    CHECK_EQ(target->block, (glm::ivec3{0, 0, 0}));
    CHECK_EQ(target->normal, (glm::ivec3{0, 0, 1}));

    auto bindings = std::make_shared<Input::InputBindings>();
    bindings->bind("remove_block", GLFW_KEY_R);
    bindings->bind("place_block", GLFW_KEY_P);
    Input::InputState input;
    input.setBindings(std::move(bindings));
    input.beginFrame();
    input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_P, GLFW_PRESS);
    input.beginFrame();
    Input::WindowState window;

    CHECK(Input::handleBlockEdits(
        input, window, targetPointer(target), world, view, placedBlock,
        Input::GameplayMutationMode::ReadWrite));
    CHECK(world.getBlock(0, 0, 0).isAir());
    CHECK(world.getBlock(0, 0, 1).isAir());
}

TEST_CASE(GameplayInput_NoTargetDoesNotEditWorld) {
    Voxel::WorldResources resources;
    const Voxel::BlockID placed = addModelBlock(
        resources,
        "invented:no_target_cube",
        {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    world.setBlock(3, 2, 1, Voxel::BlockState{placed});
    Input::InputState input = pressedEditInput("remove_block", GLFW_KEY_R);
    Input::WindowState window;

    CHECK(!Input::handleBlockEdits(
        input, window, nullptr, world, view, placed,
        Input::GameplayMutationMode::ReadWrite));
    CHECK_EQ(world.getBlock(3, 2, 1).id, placed);
}

TEST_CASE(GameplayInput_ReadWriteModeRetainsDemoMutation) {
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Asset::AssetManager assets;
    Input::CameraState camera;
    auto bindings = std::make_shared<Input::InputBindings>();
    bindings->bind("demo_spawn_entity", GLFW_KEY_F2);
    Input::InputState input;
    input.setBindings(bindings);
    input.beginFrame();
    input.handleKeyEvent(GLFW_KEY_F2, GLFW_PRESS);
    input.beginFrame();

    Input::handleDemoSpawn(
        input,
        assets,
        world,
        camera,
        Input::GameplayMutationMode::ReadWrite);

    CHECK_EQ(world.entities().size(), static_cast<size_t>(1));
}
