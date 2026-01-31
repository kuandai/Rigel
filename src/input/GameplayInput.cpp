#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "Rigel/input/GameplayInput.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldView.h"
#include "Rigel/input/keypress.h"

#include <spdlog/spdlog.h>

#if defined(RIGEL_ENABLE_IMGUI)
#include <imgui.h>
#include <imgui_impl_glfw.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace Rigel::Input {
namespace {

struct RaycastHit {
    glm::ivec3 block{};
    glm::ivec3 normal{};
    float distance = 0.0f;
};

bool raycastBlock(const Voxel::World& world,
                  const glm::vec3& origin,
                  const glm::vec3& direction,
                  float maxDistance,
                  RaycastHit& outHit) {
    float dirLen = glm::length(direction);
    if (dirLen <= 0.0001f) {
        return false;
    }

    glm::vec3 dir = direction / dirLen;
    glm::ivec3 blockPos(
        static_cast<int>(std::floor(origin.x)),
        static_cast<int>(std::floor(origin.y)),
        static_cast<int>(std::floor(origin.z))
    );

    glm::ivec3 step(0);
    glm::vec3 tMax(0.0f);
    glm::vec3 tDelta(0.0f);

    auto setupAxis = [](float originCoord, float dirCoord, int blockCoord,
                        int& stepOut, float& tMaxOut, float& tDeltaOut) {
        if (dirCoord > 0.0f) {
            stepOut = 1;
            float nextBoundary = static_cast<float>(blockCoord + 1);
            tMaxOut = (nextBoundary - originCoord) / dirCoord;
            tDeltaOut = 1.0f / dirCoord;
        } else if (dirCoord < 0.0f) {
            stepOut = -1;
            float nextBoundary = static_cast<float>(blockCoord);
            tMaxOut = (originCoord - nextBoundary) / -dirCoord;
            tDeltaOut = 1.0f / -dirCoord;
        } else {
            stepOut = 0;
            tMaxOut = std::numeric_limits<float>::infinity();
            tDeltaOut = std::numeric_limits<float>::infinity();
        }
    };

    setupAxis(origin.x, dir.x, blockPos.x, step.x, tMax.x, tDelta.x);
    setupAxis(origin.y, dir.y, blockPos.y, step.y, tMax.y, tDelta.y);
    setupAxis(origin.z, dir.z, blockPos.z, step.z, tMax.z, tDelta.z);

    glm::ivec3 normal(0);
    float t = 0.0f;

    while (t <= maxDistance) {
        if (!world.getBlock(blockPos.x, blockPos.y, blockPos.z).isAir()) {
            outHit.block = blockPos;
            outHit.normal = normal;
            outHit.distance = t;
            return true;
        }

        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                blockPos.x += step.x;
                t = tMax.x;
                tMax.x += tDelta.x;
                normal = glm::ivec3(-step.x, 0, 0);
            } else {
                blockPos.z += step.z;
                t = tMax.z;
                tMax.z += tDelta.z;
                normal = glm::ivec3(0, 0, -step.z);
            }
        } else {
            if (tMax.y < tMax.z) {
                blockPos.y += step.y;
                t = tMax.y;
                tMax.y += tDelta.y;
                normal = glm::ivec3(0, -step.y, 0);
            } else {
                blockPos.z += step.z;
                t = tMax.z;
                tMax.z += tDelta.z;
                normal = glm::ivec3(0, 0, -step.z);
            }
        }
    }

    return false;
}

} // namespace

void setCursorCaptured(WindowState& window, bool captured) {
    window.cursorCaptured = captured;
    if (!window.window) {
        return;
    }

    glfwSetInputMode(window.window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window.window, GLFW_RAW_MOUSE_MOTION,
                         captured ? GLFW_TRUE : GLFW_FALSE);
    }
    window.firstMouse = true;
}

void registerWindowCallbacks(GLFWwindow* window, InputCallbackContext& context) {
    glfwSetWindowUserPointer(window, &context);
    glfwSetKeyCallback(window, keyCallback);
    glfwSetCharCallback(window, [](GLFWwindow* cbWindow, unsigned int c) {
#if defined(RIGEL_ENABLE_IMGUI)
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplGlfw_CharCallback(cbWindow, c);
        }
#else
        (void)cbWindow;
        (void)c;
#endif
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* cbWindow, int button, int action, int mods) {
#if defined(RIGEL_ENABLE_IMGUI)
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplGlfw_MouseButtonCallback(cbWindow, button, action, mods);
        }
#else
        (void)cbWindow;
        (void)button;
        (void)action;
        (void)mods;
#endif
    });
    glfwSetScrollCallback(window, [](GLFWwindow* cbWindow, double xoffset, double yoffset) {
#if defined(RIGEL_ENABLE_IMGUI)
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplGlfw_ScrollCallback(cbWindow, xoffset, yoffset);
        }
#else
        (void)cbWindow;
        (void)xoffset;
        (void)yoffset;
#endif
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow* cbWindow, double xpos, double ypos) {
#if defined(RIGEL_ENABLE_IMGUI)
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplGlfw_CursorPosCallback(cbWindow, xpos, ypos);
        }
#endif
        auto* ctx = static_cast<InputCallbackContext*>(glfwGetWindowUserPointer(cbWindow));
        if (!ctx || !ctx->window || !ctx->camera) {
            return;
        }
        WindowState& windowState = *ctx->window;
        CameraState& camera = *ctx->camera;

        if (!windowState.cursorCaptured) {
            return;
        }

        if (windowState.firstMouse) {
            windowState.lastMouseX = xpos;
            windowState.lastMouseY = ypos;
            windowState.firstMouse = false;
            return;
        }

        double xoffset = xpos - windowState.lastMouseX;
        double yoffset = ypos - windowState.lastMouseY;
        windowState.lastMouseX = xpos;
        windowState.lastMouseY = ypos;

        camera.yaw += static_cast<float>(xoffset) * camera.mouseSensitivity;
        camera.pitch -= static_cast<float>(yoffset) * camera.mouseSensitivity;
        camera.pitch = std::clamp(camera.pitch, -89.0f, 89.0f);
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* cbWindow, int focused) {
        auto* ctx = static_cast<InputCallbackContext*>(glfwGetWindowUserPointer(cbWindow));
        if (!ctx || !ctx->window) {
            return;
        }
        WindowState& windowState = *ctx->window;
        windowState.windowFocused = focused != 0;
        windowState.pendingTimeReset = true;
        if (focused) {
            if (windowState.cursorCaptured) {
                setCursorCaptured(windowState, true);
            }
        } else {
            windowState.firstMouse = true;
        }
    });
}

void loadInputBindings(Asset::AssetManager& assets, InputState& input) {
    if (assets.exists("input/default")) {
        auto bindingsHandle = assets.get<InputBindings>("input/default");
        input.bindings = bindingsHandle.shared();
    }
    if (!input.bindings) {
        input.bindings = std::make_shared<InputBindings>();
    }
    ensureDefaultBindings(*input.bindings);
    input.dispatcher.setBindings(input.bindings);
}

void ensureDefaultBindings(InputBindings& bindings) {
    if (!bindings.hasAction("debug_overlay")) {
        bindings.bind("debug_overlay", GLFW_KEY_F1);
    }
    if (!bindings.hasAction("imgui_overlay")) {
        bindings.bind("imgui_overlay", GLFW_KEY_F3);
    }
    if (!bindings.hasAction("move_forward")) {
        bindings.bind("move_forward", GLFW_KEY_W);
    }
    if (!bindings.hasAction("move_backward")) {
        bindings.bind("move_backward", GLFW_KEY_S);
    }
    if (!bindings.hasAction("move_left")) {
        bindings.bind("move_left", GLFW_KEY_A);
    }
    if (!bindings.hasAction("move_right")) {
        bindings.bind("move_right", GLFW_KEY_D);
    }
    if (!bindings.hasAction("move_up")) {
        bindings.bind("move_up", GLFW_KEY_SPACE);
    }
    if (!bindings.hasAction("move_down")) {
        bindings.bind("move_down", GLFW_KEY_LEFT_CONTROL);
    }
    if (!bindings.hasAction("sprint")) {
        bindings.bind("sprint", GLFW_KEY_LEFT_SHIFT);
    }
    if (!bindings.hasAction("toggle_mouse_capture")) {
        bindings.bind("toggle_mouse_capture", GLFW_KEY_TAB);
    }
    if (!bindings.hasAction("demo_spawn_entity")) {
        bindings.bind("demo_spawn_entity", GLFW_KEY_F2);
    }
}

void attachDebugOverlayListener(InputState& input, bool* overlayEnabled) {
    input.debugOverlayListener.enabled = overlayEnabled;
    input.dispatcher.addListener(&input.debugOverlayListener);
}

void attachImGuiOverlayListener(InputState& input, bool* overlayEnabled) {
    input.imguiOverlayListener.enabled = overlayEnabled;
    input.dispatcher.addListener(&input.imguiOverlayListener);
}

void updateCamera(const InputState& input, CameraState& camera, float dt) {
    if (dt <= 0.0f) {
        return;
    }

    camera.pitch = std::clamp(camera.pitch, -89.0f, 89.0f);

    float yawRad = glm::radians(camera.yaw);
    float pitchRad = glm::radians(camera.pitch);
    glm::vec3 forward{
        std::cos(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::sin(yawRad) * std::cos(pitchRad)
    };
    forward = glm::normalize(forward);

    glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));

    glm::vec3 move(0.0f);
    if (input.dispatcher.isActionPressed("move_forward")) {
        move += forward;
    }
    if (input.dispatcher.isActionPressed("move_backward")) {
        move -= forward;
    }
    if (input.dispatcher.isActionPressed("move_right")) {
        move += right;
    }
    if (input.dispatcher.isActionPressed("move_left")) {
        move -= right;
    }
    if (input.dispatcher.isActionPressed("move_up")) {
        move += worldUp;
    }
    if (input.dispatcher.isActionPressed("move_down")) {
        move -= worldUp;
    }

    float speed = camera.moveSpeed;
    if (input.dispatcher.isActionPressed("sprint")) {
        speed *= 2.0f;
    }

    if (glm::length(move) > 0.0f) {
        camera.position += glm::normalize(move) * speed * dt;
    }

    camera.forward = forward;
    camera.right = right;
    camera.up = up;
    camera.target = camera.position + forward;
}

void handleDemoSpawn(InputState& input,
                     Asset::AssetManager& assets,
                     Voxel::World& world,
                     const CameraState& camera) {
    if (!input.dispatcher.isActionJustPressed("demo_spawn_entity")) {
        return;
    }

    auto entity = std::make_unique<Entity::Entity>("rigel:demo_entity");
    glm::vec3 spawnPos = camera.position + camera.forward * 2.0f;
    spawnPos.y += 0.5f;
    entity->setPosition(spawnPos);
    if (assets.exists("entity_models/model_drone_interceptor")) {
        auto model = assets.get<Entity::EntityModelAsset>("entity_models/model_drone_interceptor");
        entity->setModel(std::move(model));
    }
    Entity::EntityId id = world.entities().spawn(std::move(entity));
    if (!id.isNull()) {
        spdlog::info("Spawned demo entity {}:{}:{} at {:.2f}, {:.2f}, {:.2f}",
                     id.time, id.random, id.counter,
                     spawnPos.x, spawnPos.y, spawnPos.z);
    } else {
        spdlog::warn("Failed to spawn demo entity");
    }
}

void handleBlockEdits(InputState& input,
                      WindowState& window,
                      const CameraState& camera,
                      Voxel::World& world,
                      Voxel::WorldView& worldView,
                      Voxel::BlockID placeBlock) {
    bool leftDown = glfwGetMouseButton(window.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool rightDown = glfwGetMouseButton(window.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (window.cursorCaptured) {
        auto rebuildEditedChunk = [&](const glm::ivec3& worldPos) {
            Voxel::ChunkCoord coord = Voxel::worldToChunk(worldPos.x, worldPos.y, worldPos.z);
            int lx = 0;
            int ly = 0;
            int lz = 0;
            Voxel::worldToLocal(worldPos.x, worldPos.y, worldPos.z, lx, ly, lz);
            worldView.rebuildChunkMesh(coord);
            if (lx == 0) {
                worldView.rebuildChunkMesh(coord.offset(-1, 0, 0));
            } else if (lx == Voxel::Chunk::SIZE - 1) {
                worldView.rebuildChunkMesh(coord.offset(1, 0, 0));
            }
            if (ly == 0) {
                worldView.rebuildChunkMesh(coord.offset(0, -1, 0));
            } else if (ly == Voxel::Chunk::SIZE - 1) {
                worldView.rebuildChunkMesh(coord.offset(0, 1, 0));
            }
            if (lz == 0) {
                worldView.rebuildChunkMesh(coord.offset(0, 0, -1));
            } else if (lz == Voxel::Chunk::SIZE - 1) {
                worldView.rebuildChunkMesh(coord.offset(0, 0, 1));
            }
        };

        const float interactDistance = 8.0f;
        RaycastHit hit;
        if (leftDown && !input.lastLeftDown) {
            if (raycastBlock(world, camera.position, camera.forward,
                             interactDistance, hit)) {
                world.setBlock(hit.block.x, hit.block.y, hit.block.z, Voxel::BlockState{});
                rebuildEditedChunk(hit.block);
            }
        }
        if (rightDown && !input.lastRightDown) {
            if (raycastBlock(world, camera.position, camera.forward,
                             interactDistance, hit)) {
                glm::ivec3 placePos = hit.block + hit.normal;
                if (hit.normal != glm::ivec3(0) &&
                    placeBlock != Voxel::BlockRegistry::airId() &&
                    world.getBlock(placePos.x, placePos.y, placePos.z).isAir()) {
                    Voxel::BlockState state;
                    state.id = placeBlock;
                    world.setBlock(placePos.x, placePos.y, placePos.z, state);
                    rebuildEditedChunk(placePos);
                }
            }
        }
    }
    input.lastLeftDown = leftDown;
    input.lastRightDown = rightDown;
}

} // namespace Rigel::Input
