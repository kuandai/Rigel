#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"
#include "Rigel/input/GameplayInput.h"
#include "Rigel/Asset/AssetManager.h"

#include <GLFW/glfw3.h>

#include <memory>

using namespace Rigel;

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
    solid.isSolid = true;
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
    Input::handleBlockEdits(
        input, window, camera, world, view, solidId,
        Input::GameplayMutationMode::ReadWrite);
    CHECK_EQ(world.getBlock(0, 0, 0).id, solidId);

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
    input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    input.beginFrame();
    Input::handleBlockEdits(
        input, window, camera, world, view, solidId,
        Input::GameplayMutationMode::ReadWrite);
    CHECK(world.getBlock(0, 0, 0).isAir());

    input.handleKeyEvent(GLFW_KEY_R, GLFW_RELEASE);
    input.beginFrame();
    world.setBlock(0, 0, 0, Voxel::BlockState{solidId});
    input.handleKeyEvent(GLFW_KEY_P, GLFW_PRESS);
    input.beginFrame();
    Input::handleBlockEdits(
        input, window, camera, world, view, solidId,
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
        camera,
        world,
        view,
        solidId,
        Input::GameplayMutationMode::ReadOnly);

    CHECK_NE(camera.position, initialCameraPosition);
    CHECK_EQ(world.getBlock(0, 0, 0).id, solidId);
    CHECK(world.getBlock(0, 0, 1).isAir());
    CHECK_EQ(world.entities().size(), static_cast<size_t>(0));
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
