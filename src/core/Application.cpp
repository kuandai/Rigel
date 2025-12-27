#include "Rigel/Application.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include <spdlog/spdlog.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Rigel/input/keypress.h"
#include "Rigel/version.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Rigel {

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

struct Application::Impl {
    GLFWwindow* window = nullptr;
    Asset::AssetManager assets;
    Voxel::World world;
    bool worldReady = false;
    glm::vec3 cameraPos = glm::vec3(48.0f, 32.0f, 48.0f);
    glm::vec3 cameraTarget = glm::vec3(8.0f, 0.0f, 8.0f);
    glm::vec3 cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
    float cameraYaw = -135.0f;
    float cameraPitch = -20.0f;
    float moveSpeed = 10.0f;
    float mouseSensitivity = 0.12f;
    bool cursorCaptured = true;
    bool firstMouse = true;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    double lastTime = 0.0;
    bool benchmarkEnabled = false;
    double benchmarkStartTime = 0.0;
    Voxel::ChunkBenchmarkStats benchmark;
    Voxel::BlockID placeBlock = Voxel::BlockRegistry::airId();
    bool lastLeftDown = false;
    bool lastRightDown = false;

    void setCursorCaptured(bool captured) {
        cursorCaptured = captured;
        if (!window) {
            return;
        }

        glfwSetInputMode(window, GLFW_CURSOR,
                         captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                             captured ? GLFW_TRUE : GLFW_FALSE);
        }
        firstMouse = true;
    }

    void updateCamera(float dt) {
        if (dt <= 0.0f) {
            return;
        }

        cameraPitch = std::clamp(cameraPitch, -89.0f, 89.0f);

        float yawRad = glm::radians(cameraYaw);
        float pitchRad = glm::radians(cameraPitch);
        glm::vec3 forward{
            std::cos(yawRad) * std::cos(pitchRad),
            std::sin(pitchRad),
            std::sin(yawRad) * std::cos(pitchRad)
        };
        forward = glm::normalize(forward);

        glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));

        glm::vec3 move(0.0f);
        if (isKeyPressed(GLFW_KEY_W)) {
            move += forward;
        }
        if (isKeyPressed(GLFW_KEY_S)) {
            move -= forward;
        }
        if (isKeyPressed(GLFW_KEY_D)) {
            move += right;
        }
        if (isKeyPressed(GLFW_KEY_A)) {
            move -= right;
        }
        if (isKeyPressed(GLFW_KEY_SPACE)) {
            move += worldUp;
        }
        if (isKeyPressed(GLFW_KEY_LEFT_CONTROL) || isKeyPressed(GLFW_KEY_RIGHT_CONTROL)) {
            move -= worldUp;
        }

        float speed = moveSpeed;
        if (isKeyPressed(GLFW_KEY_LEFT_SHIFT)) {
            speed *= 2.0f;
        }

        if (glm::length(move) > 0.0f) {
            cameraPos += glm::normalize(move) * speed * dt;
        }

        cameraForward = forward;
        cameraTarget = cameraPos + forward;
    }

};

Application::Application() : m_impl(std::make_unique<Impl>()) {
    #ifdef DEBUG
    spdlog::info("Rigel v{} Developer Preview", RIGEL_VERSION);
    #else
    spdlog::info("Rigel v{}", RIGEL_VERSION);
    #endif

    // Initialize GLFW
    if (!glfwInit()) {
        spdlog::error("GLFW initialization failed");
        throw std::runtime_error("GLFW initialization failed");
    }
    spdlog::info("GLFW initialized successfully");

    // Create a simple GLFW window
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);

    m_impl->window = glfwCreateWindow(800, 600, "Rigel", nullptr, nullptr);
    if (!m_impl->window) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_impl->window);

    // Initialize GLEW
    if (glewInit() != GLEW_OK) {
        spdlog::error("GLEW initialization failed");
        glfwDestroyWindow(m_impl->window);
        glfwTerminate();
        throw std::runtime_error("GLEW initialization failed");
    }
    spdlog::info("GLEW initialized successfully");

    // Print OpenGL version
    spdlog::info("OpenGL Version: {}", (char*)glGetString(GL_VERSION));

    // Set initial viewport
    glViewport(0, 0, 800, 600);

    // Set Callbacks
    glfwSetFramebufferSizeCallback(m_impl->window, [](GLFWwindow* window, int width, int height)-> void {
        glViewport(0, 0, width, height);
    });
    glfwSetKeyCallback(m_impl->window, Rigel::keyCallback);
    glfwSetWindowUserPointer(m_impl->window, m_impl.get());
    m_impl->setCursorCaptured(true);
    glfwSetCursorPosCallback(m_impl->window, [](GLFWwindow* window, double xpos, double ypos) {
        auto* impl = static_cast<Application::Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) {
            return;
        }

        if (!impl->cursorCaptured) {
            return;
        }

        if (impl->firstMouse) {
            impl->lastMouseX = xpos;
            impl->lastMouseY = ypos;
            impl->firstMouse = false;
            return;
        }

        double xoffset = xpos - impl->lastMouseX;
        double yoffset = ypos - impl->lastMouseY;
        impl->lastMouseX = xpos;
        impl->lastMouseY = ypos;

        impl->cameraYaw += static_cast<float>(xoffset) * impl->mouseSensitivity;
        impl->cameraPitch -= static_cast<float>(yoffset) * impl->mouseSensitivity;
        impl->cameraPitch = std::clamp(impl->cameraPitch, -89.0f, 89.0f);
    });
    glfwSetWindowFocusCallback(m_impl->window, [](GLFWwindow* window, int focused) {
        auto* impl = static_cast<Application::Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) {
            return;
        }

        if (focused) {
            impl->setCursorCaptured(true);
        } else {
            impl->firstMouse = true;
        }
    });

    const char* benchEnv = std::getenv("RIGEL_CHUNK_BENCH");
    if (benchEnv && benchEnv[0] != '\0' && benchEnv[0] != '0') {
        m_impl->benchmarkEnabled = true;
        spdlog::info("Chunk benchmark enabled");
    }

    try {
        m_impl->assets.loadManifest("manifest.yaml");
        m_impl->world.initialize(m_impl->assets);

        Voxel::ConfigProvider configProvider;
        configProvider.addSource(
            std::make_unique<Voxel::EmbeddedConfigSource>(m_impl->assets, "raw/world_config")
        );
        configProvider.addSource(
            std::make_unique<Voxel::FileConfigSource>("config/world_generation.yaml")
        );
        configProvider.addSource(
            std::make_unique<Voxel::FileConfigSource>("world_generation.yaml")
        );

        Voxel::WorldGenConfig config = configProvider.loadConfig();
        if (config.solidBlock.empty()) {
            config.solidBlock = "rigel:stone";
        }
        if (config.surfaceBlock.empty()) {
            config.surfaceBlock = "rigel:grass";
        }

        auto generator = std::make_shared<Voxel::WorldGenerator>(m_impl->world.blockRegistry());
        generator->setConfig(config);
        m_impl->world.setGenerator(generator);
        m_impl->world.setStreamConfig(config.stream);
        if (m_impl->benchmarkEnabled) {
            m_impl->world.setBenchmark(&m_impl->benchmark);
        }
        auto placeId = m_impl->world.blockRegistry().findByIdentifier(config.solidBlock);
        if (!placeId) {
            placeId = m_impl->world.blockRegistry().findByIdentifier("rigel:stone");
        }
        if (placeId) {
            m_impl->placeBlock = *placeId;
        } else if (m_impl->world.blockRegistry().size() > 1) {
            m_impl->placeBlock = Voxel::BlockID{1};
        }

        m_impl->worldReady = true;
    } catch (const std::exception& e) {
        spdlog::error("Voxel bootstrap failed: {}", e.what());
    }
}

Application::~Application() {
    if (m_impl->window) {
        glfwDestroyWindow(m_impl->window);
    }
    glfwTerminate();
    spdlog::info("Application terminated successfully");
}

void Application::run() {
    m_impl->lastTime = glfwGetTime();
    if (m_impl->benchmarkEnabled) {
        m_impl->benchmarkStartTime = m_impl->lastTime;
    }

    // Render loop
    while (!glfwWindowShouldClose(m_impl->window)) {
        double now = glfwGetTime();
        float deltaTime = static_cast<float>(now - m_impl->lastTime);
        m_impl->lastTime = now;

        // Frame setup
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Flush event queue
        glfwPollEvents();
        Rigel::keyupdate();

        if (m_impl->worldReady) {
            if (m_impl->cursorCaptured &&
                glfwGetInputMode(m_impl->window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED) {
                m_impl->setCursorCaptured(true);
            }
            m_impl->updateCamera(deltaTime);

            bool leftDown = glfwGetMouseButton(m_impl->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            bool rightDown = glfwGetMouseButton(m_impl->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            if (m_impl->cursorCaptured) {
                const float interactDistance = 8.0f;
                RaycastHit hit;
                if (leftDown && !m_impl->lastLeftDown) {
                    if (raycastBlock(m_impl->world, m_impl->cameraPos, m_impl->cameraForward,
                                     interactDistance, hit)) {
                        m_impl->world.setBlock(hit.block.x, hit.block.y, hit.block.z, Voxel::BlockState{});
                    }
                }
                if (rightDown && !m_impl->lastRightDown) {
                    if (raycastBlock(m_impl->world, m_impl->cameraPos, m_impl->cameraForward,
                                     interactDistance, hit)) {
                        glm::ivec3 placePos = hit.block + hit.normal;
                        if (hit.normal != glm::ivec3(0) &&
                            m_impl->placeBlock != Voxel::BlockRegistry::airId() &&
                            m_impl->world.getBlock(placePos.x, placePos.y, placePos.z).isAir()) {
                            Voxel::BlockState state;
                            state.id = m_impl->placeBlock;
                            m_impl->world.setBlock(placePos.x, placePos.y, placePos.z, state);
                        }
                    }
                }
            }
            m_impl->lastLeftDown = leftDown;
            m_impl->lastRightDown = rightDown;

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(m_impl->window, &width, &height);
            float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

            float renderDistance = m_impl->world.renderer().config().renderDistance;
            float farPlane = std::max(500.0f, renderDistance + static_cast<float>(Voxel::Chunk::SIZE));
            glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, farPlane);
            glm::mat4 view = glm::lookAt(
                m_impl->cameraPos,
                m_impl->cameraTarget,
                glm::vec3(0.0f, 1.0f, 0.0f)
            );

            glm::mat4 viewProjection = projection * view;

            m_impl->world.updateStreaming(m_impl->cameraPos);
            m_impl->world.updateMeshes();
            m_impl->world.render(viewProjection, m_impl->cameraPos);
        }

        glfwSwapBuffers(m_impl->window);

        // Exit on ESC
        if (isKeyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(m_impl->window, true);
        }
    }

    if (m_impl->benchmarkEnabled) {
        double endTime = glfwGetTime();
        double elapsed = endTime - m_impl->benchmarkStartTime;
        const auto& stats = m_impl->benchmark;
        double genRate = (elapsed > 0.0)
            ? static_cast<double>(stats.generatedChunks) / elapsed
            : 0.0;
        double processedRate = (elapsed > 0.0)
            ? static_cast<double>(stats.processedChunks()) / elapsed
            : 0.0;
        double meshedRate = (elapsed > 0.0)
            ? static_cast<double>(stats.meshedChunks) / elapsed
            : 0.0;
        spdlog::info(
            "Chunk benchmark (lifetime): generated {} ({:.1f}/s), processed {} ({:.1f}/s), "
            "meshed {} ({:.1f}/s), empty {}, wall {:.2f}s "
            "[gen {:.2f}s, mesh {:.2f}s, empty {:.2f}s]",
            stats.generatedChunks,
            genRate,
            stats.processedChunks(),
            processedRate,
            stats.meshedChunks,
            meshedRate,
            stats.emptyChunks,
            elapsed,
            stats.generationSeconds,
            stats.meshSeconds,
            stats.emptyMeshSeconds
        );
    }
}

} // namespace Rigel
