#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"
#include "Rigel/input/GameplayInput.h"

#include <GLFW/glfw3.h>

#include <memory>

using namespace Rigel;

TEST_CASE(GameplayInput_CursorMathUsesEffectiveSensitivityAndInvertY) {
    Input::WindowState window;
    Input::CameraState camera;
    camera.yaw = 10.0f;
    camera.pitch = 5.0f;

    Input::applyCursorPosition(window, camera, 0.5, false, 100.0, 200.0);
    CHECK_EQ(camera.yaw, 10.0f);
    CHECK_EQ(camera.pitch, 5.0f);

    Input::applyCursorPosition(window, camera, 0.5, false, 104.0, 206.0);
    CHECK_EQ(camera.yaw, 12.0f);
    CHECK_EQ(camera.pitch, 2.0f);

    Input::applyCursorPosition(window, camera, 0.25, true, 108.0, 210.0);
    CHECK_EQ(camera.yaw, 13.0f);
    CHECK_EQ(camera.pitch, 3.0f);

    window.cursorCaptured = false;
    Input::applyCursorPosition(window, camera, 1.0, true, 200.0, 300.0);
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
        input, window, camera, world, view, solidId);
    CHECK_EQ(world.getBlock(0, 0, 0).id, solidId);

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
    input.handleKeyEvent(GLFW_KEY_R, GLFW_PRESS);
    input.beginFrame();
    Input::handleBlockEdits(
        input, window, camera, world, view, solidId);
    CHECK(world.getBlock(0, 0, 0).isAir());

    input.handleKeyEvent(GLFW_KEY_R, GLFW_RELEASE);
    input.beginFrame();
    world.setBlock(0, 0, 0, Voxel::BlockState{solidId});
    input.handleKeyEvent(GLFW_KEY_P, GLFW_PRESS);
    input.beginFrame();
    Input::handleBlockEdits(
        input, window, camera, world, view, solidId);
    CHECK_EQ(world.getBlock(0, 0, 1).id, solidId);
}
