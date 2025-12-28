#include "Rigel/Application.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include <spdlog/spdlog.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/input/InputDispatcher.h"
#include "Rigel/input/keypress.h"
#include "Rigel/version.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace Rigel {

namespace {

struct RaycastHit {
    glm::ivec3 block{};
    glm::ivec3 normal{};
    float distance = 0.0f;
};

constexpr float kDebugTargetSpan = 6.0f;
constexpr float kDebugDistance = 8.0f;
constexpr float kDebugAlpha = 0.35f;
constexpr int kDebugViewportSize = 130;
constexpr int kDebugViewportMargin = 12;
constexpr int kFrameGraphSamples = 180;
constexpr float kFrameGraphMaxMs = 50.0f;
constexpr float kFrameGraphHeight = 0.28f;
constexpr float kFrameGraphBottom = -0.95f;

constexpr float kCubeVertices[] = {
    // +X
    0.5f, -0.5f, -0.5f,
    0.5f,  0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    0.5f, -0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    0.5f, -0.5f,  0.5f,
    // -X
    -0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f,
    // +Y
    -0.5f,  0.5f, -0.5f,
    0.5f,  0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,
    0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    // -Y
    -0.5f, -0.5f,  0.5f,
    0.5f, -0.5f,  0.5f,
    0.5f, -0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,
    0.5f, -0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f,
    // +Z
    -0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,
    0.5f,  0.5f,  0.5f,
    0.5f, -0.5f,  0.5f,
    // -Z
    0.5f, -0.5f, -0.5f,
    0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
    0.5f, -0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f
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
    struct DebugOverlayListener : InputListener {
        Impl* owner = nullptr;

        void onActionReleased(std::string_view action) override {
            if (!owner) {
                return;
            }
            if (action == "debug_overlay") {
                owner->debugOverlayEnabled = !owner->debugOverlayEnabled;
            }
        }
    };

    struct DebugField {
        GLuint vao = 0;
        std::array<GLuint, 4> vbos{};
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locViewProjection = -1;
        GLint locFieldOrigin = -1;
        GLint locFieldRight = -1;
        GLint locFieldUp = -1;
        GLint locFieldForward = -1;
        GLint locCellSize = -1;
        GLint locColor = -1;
        bool initialized = false;
    };

    struct FrameTimeGraph {
        GLuint vao = 0;
        GLuint vbo = 0;
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locColor = -1;
        std::vector<float> samples;
        size_t cursor = 0;
        size_t filled = 0;
        bool initialized = false;
    };

    GLFWwindow* window = nullptr;
    Asset::AssetManager assets;
    Voxel::World world;
    bool worldReady = false;
    glm::vec3 cameraPos = glm::vec3(48.0f, 32.0f, 48.0f);
    glm::vec3 cameraTarget = glm::vec3(8.0f, 0.0f, 8.0f);
    glm::vec3 cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
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
    DebugField debugField;
    FrameTimeGraph frameGraph;
    std::vector<Voxel::ChunkStreamer::DebugChunkState> debugStates;
    float debugDistance = kDebugDistance;
    bool debugOverlayEnabled = true;
    std::shared_ptr<InputBindings> inputBindings;
    InputDispatcher inputDispatcher;
    DebugOverlayListener debugOverlayListener;

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
        glm::vec3 up = glm::normalize(glm::cross(right, forward));

        glm::vec3 move(0.0f);
        if (inputDispatcher.isActionPressed("move_forward")) {
            move += forward;
        }
        if (inputDispatcher.isActionPressed("move_backward")) {
            move -= forward;
        }
        if (inputDispatcher.isActionPressed("move_right")) {
            move += right;
        }
        if (inputDispatcher.isActionPressed("move_left")) {
            move -= right;
        }
        if (inputDispatcher.isActionPressed("move_up")) {
            move += worldUp;
        }
        if (inputDispatcher.isActionPressed("move_down")) {
            move -= worldUp;
        }

        float speed = moveSpeed;
        if (inputDispatcher.isActionPressed("sprint")) {
            speed *= 2.0f;
        }

        if (glm::length(move) > 0.0f) {
            cameraPos += glm::normalize(move) * speed * dt;
        }

        cameraForward = forward;
        cameraRight = right;
        cameraUp = up;
        cameraTarget = cameraPos + forward;
    }

    void initDebugField() {
        try {
            debugField.shader = assets.get<Asset::ShaderAsset>("shaders/chunk_debug");
        } catch (const std::exception& e) {
            spdlog::warn("Debug chunk shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &debugField.vao);
        glGenBuffers(static_cast<GLsizei>(debugField.vbos.size()), debugField.vbos.data());

        glBindVertexArray(debugField.vao);

        glBindVertexArray(0);

        debugField.locViewProjection = debugField.shader->uniform("u_viewProjection");
        debugField.locFieldOrigin = debugField.shader->uniform("u_fieldOrigin");
        debugField.locFieldRight = debugField.shader->uniform("u_fieldRight");
        debugField.locFieldUp = debugField.shader->uniform("u_fieldUp");
        debugField.locFieldForward = debugField.shader->uniform("u_fieldForward");
        debugField.locCellSize = debugField.shader->uniform("u_cellSize");
        debugField.locColor = debugField.shader->uniform("u_color");

        debugField.initialized = true;
    }

    void initFrameGraph() {
        try {
            frameGraph.shader = assets.get<Asset::ShaderAsset>("shaders/frame_graph");
        } catch (const std::exception& e) {
            spdlog::warn("Frame graph shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &frameGraph.vao);
        glGenBuffers(1, &frameGraph.vbo);
        frameGraph.samples.assign(kFrameGraphSamples, 0.0f);
        frameGraph.initialized = true;
        frameGraph.locColor = frameGraph.shader->uniform("u_color");
    }

    void recordFrameTime(float seconds) {
        if (!frameGraph.initialized || seconds <= 0.0f) {
            return;
        }
        float ms = seconds * 1000.0f;
        frameGraph.samples[frameGraph.cursor] = ms;
        frameGraph.cursor = (frameGraph.cursor + 1) % frameGraph.samples.size();
        frameGraph.filled = std::min(frameGraph.samples.size(), frameGraph.filled + 1);
    }

    void renderFrameGraph() {
        if (!debugOverlayEnabled || !frameGraph.initialized || frameGraph.filled == 0) {
            return;
        }

        const size_t sampleCount = frameGraph.samples.size();
        const float barWidth = 2.0f / static_cast<float>(sampleCount);
        const float baseY = kFrameGraphBottom;
        const float topSpan = kFrameGraphHeight;

        std::vector<glm::vec2> vertices;
        vertices.reserve(frameGraph.filled * 6);

        for (size_t i = 0; i < frameGraph.filled; ++i) {
            size_t sampleIndex = (frameGraph.cursor + sampleCount - 1 - i) % sampleCount;
            float ms = frameGraph.samples[sampleIndex];
            float t = std::min(ms, kFrameGraphMaxMs) / kFrameGraphMaxMs;
            float height = t * topSpan;

            float x1 = 1.0f - static_cast<float>(i) * barWidth;
            float x0 = x1 - barWidth;
            float y0 = baseY;
            float y1 = baseY + height;

            vertices.emplace_back(x0, y0);
            vertices.emplace_back(x1, y0);
            vertices.emplace_back(x1, y1);

            vertices.emplace_back(x0, y0);
            vertices.emplace_back(x1, y1);
            vertices.emplace_back(x0, y1);
        }

        frameGraph.shader->bind();
        if (frameGraph.locColor >= 0) {
            glm::vec4 color(0.2f, 0.9f, 0.9f, 0.85f);
            glUniform4fv(frameGraph.locColor, 1, glm::value_ptr(color));
        }

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(frameGraph.vao);
        glBindBuffer(GL_ARRAY_BUFFER, frameGraph.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec2)),
                     vertices.data(),
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    void renderDebugField(const glm::vec3& viewForward,
                          int viewportWidth,
                          int viewportHeight) {
        if (!debugOverlayEnabled || !debugField.initialized) {
            return;
        }

        world.getChunkDebugStates(debugStates);
        if (debugStates.empty()) {
            return;
        }

        int radius = std::max(0, world.viewDistanceChunks());
        int diameter = radius * 2 + 1;
        if (diameter <= 0) {
            return;
        }

        float cellSize = kDebugTargetSpan / static_cast<float>(diameter);

        auto centerCoord = Voxel::worldToChunk(
            static_cast<int>(std::floor(cameraPos.x)),
            static_cast<int>(std::floor(cameraPos.y)),
            static_cast<int>(std::floor(cameraPos.z))
        );

        std::unordered_map<Voxel::ChunkCoord,
                           Voxel::ChunkStreamer::DebugState,
                           Voxel::ChunkCoordHash> stateMap;
        stateMap.reserve(debugStates.size());
        std::array<std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash>, 4> occupancy;
        for (auto& set : occupancy) {
            set.reserve(debugStates.size());
        }

        for (const auto& entry : debugStates) {
            int dx = entry.coord.x - centerCoord.x;
            int dy = entry.coord.y - centerCoord.y;
            int dz = entry.coord.z - centerCoord.z;

            if (std::abs(dx) > radius || std::abs(dy) > radius || std::abs(dz) > radius) {
                continue;
            }
            Voxel::ChunkCoord offset{dx, dy, dz};
            stateMap[offset] = entry.state;
            switch (entry.state) {
                case Voxel::ChunkStreamer::DebugState::QueuedGen:
                    occupancy[0].insert(offset);
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyData:
                    occupancy[1].insert(offset);
                    break;
                case Voxel::ChunkStreamer::DebugState::QueuedMesh:
                    occupancy[2].insert(offset);
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyMesh:
                    occupancy[3].insert(offset);
                    break;
            }
        }

        if (stateMap.empty()) {
            return;
        }

        std::array<std::vector<glm::vec3>, 4> meshVertices;
        std::array<glm::vec4, 4> colors = {
            glm::vec4(1.0f, 0.2f, 0.2f, kDebugAlpha),
            glm::vec4(1.0f, 0.9f, 0.2f, kDebugAlpha),
            glm::vec4(0.2f, 0.8f, 1.0f, kDebugAlpha),
            glm::vec4(0.2f, 1.0f, 0.3f, kDebugAlpha)
        };
        std::array<std::array<int, 3>, 6> offsets = {{
            { 1, 0, 0},
            {-1, 0, 0},
            { 0, 1, 0},
            { 0,-1, 0},
            { 0, 0, 1},
            { 0, 0,-1}
        }};

        for (const auto& [coord, state] : stateMap) {
            int stateIdx = 0;
            switch (state) {
                case Voxel::ChunkStreamer::DebugState::QueuedGen:
                    stateIdx = 0;
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyData:
                    stateIdx = 1;
                    break;
                case Voxel::ChunkStreamer::DebugState::QueuedMesh:
                    stateIdx = 2;
                    break;
                case Voxel::ChunkStreamer::DebugState::ReadyMesh:
                    stateIdx = 3;
                    break;
            }

            for (int face = 0; face < 6; ++face) {
                Voxel::ChunkCoord neighbor{
                    coord.x + offsets[face][0],
                    coord.y + offsets[face][1],
                    coord.z + offsets[face][2]
                };
                if (occupancy[stateIdx].find(neighbor) != occupancy[stateIdx].end()) {
                    continue;
                }

                size_t base = static_cast<size_t>(face) * 18;
                for (int v = 0; v < 6; ++v) {
                    float x = kCubeVertices[base + v * 3 + 0] + static_cast<float>(coord.x);
                    float y = kCubeVertices[base + v * 3 + 1] + static_cast<float>(coord.y);
                    float z = kCubeVertices[base + v * 3 + 2] + static_cast<float>(coord.z);
                    meshVertices[stateIdx].push_back(glm::vec3(x, y, z));
                }
            }
        }

        GLint previousViewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, previousViewport);

        int viewportSize = std::min(kDebugViewportSize, std::min(viewportWidth, viewportHeight));
        int marginX = std::min(kDebugViewportMargin, std::max(0, viewportWidth - viewportSize));
        int marginY = std::min(kDebugViewportMargin, std::max(0, viewportHeight - viewportSize));
        int viewportX = marginX;
        int viewportY = std::max(0, viewportHeight - viewportSize - marginY);
        glViewport(viewportX, viewportY, viewportSize, viewportSize);

        float renderDistance = world.renderConfig().renderDistance;
        float farPlane = std::max(500.0f, renderDistance + static_cast<float>(Voxel::Chunk::SIZE));
        glm::mat4 debugProjection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, farPlane);
        glm::mat4 debugView = glm::lookAt(
            cameraPos,
            cameraTarget,
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        glm::mat4 debugViewProjection = debugProjection * debugView;

        debugField.shader->bind();
        if (debugField.locViewProjection >= 0) {
            glUniformMatrix4fv(debugField.locViewProjection, 1, GL_FALSE, glm::value_ptr(debugViewProjection));
        }
        glm::vec3 fieldOrigin = cameraPos + viewForward * debugDistance;
        if (debugField.locFieldOrigin >= 0) {
            glUniform3fv(debugField.locFieldOrigin, 1, glm::value_ptr(fieldOrigin));
        }
        if (debugField.locFieldRight >= 0) {
            glUniform3fv(debugField.locFieldRight, 1, glm::value_ptr(glm::vec3(1.0f, 0.0f, 0.0f)));
        }
        if (debugField.locFieldUp >= 0) {
            glUniform3fv(debugField.locFieldUp, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));
        }
        if (debugField.locFieldForward >= 0) {
            glUniform3fv(debugField.locFieldForward, 1, glm::value_ptr(glm::vec3(0.0f, 0.0f, 1.0f)));
        }
        if (debugField.locCellSize >= 0) {
            glUniform1f(debugField.locCellSize, cellSize);
        }

        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(debugField.vao);
        for (size_t i = 0; i < meshVertices.size(); ++i) {
            if (meshVertices[i].empty()) {
                continue;
            }

            if (debugField.locColor >= 0) {
                glUniform4fv(debugField.locColor, 1, glm::value_ptr(colors[i]));
            }

            glBindBuffer(GL_ARRAY_BUFFER, debugField.vbos[i]);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(meshVertices[i].size() * sizeof(glm::vec3)),
                         meshVertices[i].data(),
                         GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(meshVertices[i].size()));
        }

        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(0);
        glUseProgram(0);
        glViewport(previousViewport[0],
                   previousViewport[1],
                   previousViewport[2],
                   previousViewport[3]);
    }

};

Application::Application() : m_impl(std::make_unique<Impl>()) {
    m_impl->debugOverlayListener.owner = m_impl.get();
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
        m_impl->assets.registerLoader("input", std::make_unique<InputBindingsLoader>());
        m_impl->world.initialize(m_impl->assets);

        if (m_impl->assets.exists("input/default")) {
            auto bindingsHandle = m_impl->assets.get<InputBindings>("input/default");
            m_impl->inputBindings = bindingsHandle.shared();
        }
        if (!m_impl->inputBindings) {
            m_impl->inputBindings = std::make_shared<InputBindings>();
        }
        if (!m_impl->inputBindings->hasAction("debug_overlay")) {
            m_impl->inputBindings->bind("debug_overlay", GLFW_KEY_F1);
        }
        if (!m_impl->inputBindings->hasAction("move_forward")) {
            m_impl->inputBindings->bind("move_forward", GLFW_KEY_W);
        }
        if (!m_impl->inputBindings->hasAction("move_backward")) {
            m_impl->inputBindings->bind("move_backward", GLFW_KEY_S);
        }
        if (!m_impl->inputBindings->hasAction("move_left")) {
            m_impl->inputBindings->bind("move_left", GLFW_KEY_A);
        }
        if (!m_impl->inputBindings->hasAction("move_right")) {
            m_impl->inputBindings->bind("move_right", GLFW_KEY_D);
        }
        if (!m_impl->inputBindings->hasAction("move_up")) {
            m_impl->inputBindings->bind("move_up", GLFW_KEY_SPACE);
        }
        if (!m_impl->inputBindings->hasAction("move_down")) {
            m_impl->inputBindings->bind("move_down", GLFW_KEY_LEFT_CONTROL);
        }
        if (!m_impl->inputBindings->hasAction("sprint")) {
            m_impl->inputBindings->bind("sprint", GLFW_KEY_LEFT_SHIFT);
        }
        if (!m_impl->inputBindings->hasAction("exit")) {
            m_impl->inputBindings->bind("exit", GLFW_KEY_ESCAPE);
        }
        m_impl->inputDispatcher.setBindings(m_impl->inputBindings);
        m_impl->inputDispatcher.addListener(&m_impl->debugOverlayListener);

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

        m_impl->initDebugField();
        m_impl->initFrameGraph();
        m_impl->worldReady = true;
    } catch (const std::exception& e) {
        spdlog::error("Voxel bootstrap failed: {}", e.what());
    }
}

Application::~Application() {
    if (m_impl && m_impl->window) {
        glfwMakeContextCurrent(m_impl->window);

        if (m_impl->debugField.initialized) {
            if (m_impl->debugField.vao != 0) {
                glDeleteVertexArrays(1, &m_impl->debugField.vao);
                m_impl->debugField.vao = 0;
            }
            for (GLuint& vbo : m_impl->debugField.vbos) {
                if (vbo != 0) {
                    glDeleteBuffers(1, &vbo);
                    vbo = 0;
                }
            }
            m_impl->debugField.initialized = false;
        }
        if (m_impl->frameGraph.initialized) {
            if (m_impl->frameGraph.vao != 0) {
                glDeleteVertexArrays(1, &m_impl->frameGraph.vao);
                m_impl->frameGraph.vao = 0;
            }
            if (m_impl->frameGraph.vbo != 0) {
                glDeleteBuffers(1, &m_impl->frameGraph.vbo);
                m_impl->frameGraph.vbo = 0;
            }
            m_impl->frameGraph.initialized = false;
        }

        m_impl->world.clear();
        m_impl->world.releaseRenderResources();
        m_impl->assets.clearCache();

        glfwDestroyWindow(m_impl->window);
        m_impl->window = nullptr;
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
        m_impl->recordFrameTime(deltaTime);

        // Frame setup
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Flush event queue
        glfwPollEvents();
        Rigel::keyupdate();
        m_impl->inputDispatcher.update();

        if (m_impl->worldReady) {
            if (m_impl->cursorCaptured &&
                glfwGetInputMode(m_impl->window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED) {
                m_impl->setCursorCaptured(true);
            }
            m_impl->updateCamera(deltaTime);

            bool leftDown = glfwGetMouseButton(m_impl->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            bool rightDown = glfwGetMouseButton(m_impl->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            if (m_impl->cursorCaptured) {
                auto rebuildEditedChunk = [&](const glm::ivec3& worldPos) {
                    Voxel::ChunkCoord coord = Voxel::worldToChunk(worldPos.x, worldPos.y, worldPos.z);
                    int lx = 0;
                    int ly = 0;
                    int lz = 0;
                    Voxel::worldToLocal(worldPos.x, worldPos.y, worldPos.z, lx, ly, lz);
                    m_impl->world.rebuildChunkMesh(coord);
                    if (lx == 0) {
                        m_impl->world.rebuildChunkMesh(coord.offset(-1, 0, 0));
                    } else if (lx == Voxel::Chunk::SIZE - 1) {
                        m_impl->world.rebuildChunkMesh(coord.offset(1, 0, 0));
                    }
                    if (ly == 0) {
                        m_impl->world.rebuildChunkMesh(coord.offset(0, -1, 0));
                    } else if (ly == Voxel::Chunk::SIZE - 1) {
                        m_impl->world.rebuildChunkMesh(coord.offset(0, 1, 0));
                    }
                    if (lz == 0) {
                        m_impl->world.rebuildChunkMesh(coord.offset(0, 0, -1));
                    } else if (lz == Voxel::Chunk::SIZE - 1) {
                        m_impl->world.rebuildChunkMesh(coord.offset(0, 0, 1));
                    }
                };

                const float interactDistance = 8.0f;
                RaycastHit hit;
                if (leftDown && !m_impl->lastLeftDown) {
                    if (raycastBlock(m_impl->world, m_impl->cameraPos, m_impl->cameraForward,
                                     interactDistance, hit)) {
                        m_impl->world.setBlock(hit.block.x, hit.block.y, hit.block.z, Voxel::BlockState{});
                        rebuildEditedChunk(hit.block);
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
                            rebuildEditedChunk(placePos);
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

            float renderDistance = m_impl->world.renderConfig().renderDistance;
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
            m_impl->renderDebugField(m_impl->cameraForward,
                                     width,
                                     height);
            m_impl->renderFrameGraph();
        }

        glfwSwapBuffers(m_impl->window);

        // Exit on ESC
        if (m_impl->inputDispatcher.isActionPressed("exit")) {
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
