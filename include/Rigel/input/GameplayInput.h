#pragma once

#include "Rigel/Voxel/Block.h"
#include "Rigel/input/InputBindings.h"
#include "Rigel/input/InputState.h"

#include <glm/glm.hpp>

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

struct ImGuiOverlayListener : InputListener {
    bool* enabled = nullptr;

    void onActionReleased(std::string_view action) override {
        if (!enabled) {
            return;
        }
        if (action == "imgui_overlay") {
            *enabled = !*enabled;
        }
    }
};

struct InputCallbackContext {
    InputState* input = nullptr;
    WindowState* window = nullptr;
    CameraState* camera = nullptr;
};

void setCursorCaptured(WindowState& window, bool captured);

/// Window-local point used to pin a captured cursor so it cannot walk off-screen.
void capturedCursorCenter(int windowWidth, int windowHeight, double& x, double& y);

/// Apply one cursor-position sample to yaw/pitch. The first sample after capture
/// or a cursor warp is stored as the origin and does not rotate the camera.
void applyCapturedCursorPosition(WindowState& window, CameraState& camera,
                                 double xpos, double ypos);

/// Keep a captured cursor hidden and inside the window. GLFW 3.3's disabled
/// cursor can still leave the screen on macOS, so this falls back to hiding
/// the cursor and warping it to the window center.
void maintainCursorLock(WindowState& window);

void registerWindowCallbacks(GLFWwindow* window, InputCallbackContext& context);

void loadInputBindings(Asset::AssetManager& assets, InputState& input);

void ensureDefaultBindings(InputBindings& bindings);

void updateCamera(const InputState& input, CameraState& camera, float dt);

void handleDemoSpawn(const InputState& input,
                     Asset::AssetManager& assets,
                     Voxel::World& world,
                     const CameraState& camera);

void handleBlockEdits(const InputState& input,
                      const WindowState& window,
                      const CameraState& camera,
                      Voxel::World& world,
                      Voxel::WorldView& worldView,
                      Voxel::BlockID placeBlock);

} // namespace Input
} // namespace Rigel
