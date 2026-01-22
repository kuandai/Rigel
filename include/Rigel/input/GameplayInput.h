#pragma once

#include "Rigel/Voxel/Block.h"
#include "Rigel/input/InputBindings.h"
#include "Rigel/input/InputDispatcher.h"

#include <glm/glm.hpp>

#include <memory>

struct GLFWwindow;

namespace Rigel {
namespace Asset { class AssetManager; }
namespace Voxel { class World; class WorldView; }

namespace Input {

struct WindowState {
    GLFWwindow* window = nullptr;
    bool cursorCaptured = true;
    bool firstMouse = true;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    bool windowFocused = true;
    bool pendingTimeReset = false;
};

struct CameraState {
    glm::vec3 position = glm::vec3(48.0f, 32.0f, 48.0f);
    glm::vec3 target = glm::vec3(8.0f, 0.0f, 8.0f);
    glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 right = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    float yaw = -135.0f;
    float pitch = -20.0f;
    float moveSpeed = 10.0f;
    float mouseSensitivity = 0.12f;
};

struct DebugOverlayListener : InputListener {
    bool* enabled = nullptr;

    void onActionReleased(std::string_view action) override {
        if (!enabled) {
            return;
        }
        if (action == "debug_overlay") {
            *enabled = !*enabled;
        }
    }
};

struct InputState {
    std::shared_ptr<InputBindings> bindings;
    InputDispatcher dispatcher;
    DebugOverlayListener debugOverlayListener;
    bool lastLeftDown = false;
    bool lastRightDown = false;
};

struct InputCallbackContext {
    WindowState* window = nullptr;
    CameraState* camera = nullptr;
};

void setCursorCaptured(WindowState& window, bool captured);

void registerWindowCallbacks(GLFWwindow* window, InputCallbackContext& context);

void loadInputBindings(Asset::AssetManager& assets, InputState& input);

void ensureDefaultBindings(InputBindings& bindings);

void attachDebugOverlayListener(InputState& input, bool* overlayEnabled);

void updateCamera(const InputState& input, CameraState& camera, float dt);

void handleDemoSpawn(InputState& input,
                     Asset::AssetManager& assets,
                     Voxel::World& world,
                     const CameraState& camera);

void handleBlockEdits(InputState& input,
                      WindowState& window,
                      const CameraState& camera,
                      Voxel::World& world,
                      Voxel::WorldView& worldView,
                      Voxel::BlockID placeBlock);

} // namespace Input
} // namespace Rigel
