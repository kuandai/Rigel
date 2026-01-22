#include "Rigel/Render/DebugOverlay.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldView.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Rigel::Render {
namespace {

constexpr float kDebugTargetSpan = 6.0f;
constexpr float kDebugAlpha = 0.35f;
constexpr int kDebugViewportSize = 130;
constexpr int kDebugViewportMargin = 12;
constexpr int kFrameGraphSamples = 180;
constexpr float kFrameGraphMaxMs = 50.0f;
constexpr float kFrameGraphHeight = 0.28f;
constexpr float kFrameGraphBottom = -0.95f;
constexpr int kProfilerMaxBars = 6;
constexpr float kProfilerBarHeight = 0.05f;
constexpr float kProfilerBarGap = 0.01f;
constexpr float kProfilerLeft = -0.95f;
constexpr float kProfilerRight = 0.95f;
constexpr float kProfilerBottom = -0.55f;

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

} // namespace

void initDebugField(DebugState& debug, Asset::AssetManager& assets) {
    try {
        debug.field.shader = assets.get<Asset::ShaderAsset>("shaders/chunk_debug");
    } catch (const std::exception& e) {
        spdlog::warn("Debug chunk shader unavailable: {}", e.what());
        return;
    }

    glGenVertexArrays(1, &debug.field.vao);
    glGenBuffers(static_cast<GLsizei>(debug.field.vbos.size()), debug.field.vbos.data());

    glBindVertexArray(debug.field.vao);
    glBindVertexArray(0);

    debug.field.locViewProjection = debug.field.shader->uniform("u_viewProjection");
    debug.field.locFieldOrigin = debug.field.shader->uniform("u_fieldOrigin");
    debug.field.locFieldRight = debug.field.shader->uniform("u_fieldRight");
    debug.field.locFieldUp = debug.field.shader->uniform("u_fieldUp");
    debug.field.locFieldForward = debug.field.shader->uniform("u_fieldForward");
    debug.field.locCellSize = debug.field.shader->uniform("u_cellSize");
    debug.field.locColor = debug.field.shader->uniform("u_color");

    debug.field.initialized = true;
}

void initFrameGraph(DebugState& debug, Asset::AssetManager& assets) {
    try {
        debug.frameGraph.shader = assets.get<Asset::ShaderAsset>("shaders/frame_graph");
    } catch (const std::exception& e) {
        spdlog::warn("Frame graph shader unavailable: {}", e.what());
        return;
    }

    glGenVertexArrays(1, &debug.frameGraph.vao);
    glGenBuffers(1, &debug.frameGraph.vbo);
    debug.frameGraph.samples.assign(kFrameGraphSamples, 0.0f);
    debug.frameGraph.initialized = true;
    debug.frameGraph.locColor = debug.frameGraph.shader->uniform("u_color");
}

void initEntityDebug(DebugState& debug, Asset::AssetManager& assets) {
    try {
        debug.entityDebug.shader = assets.get<Asset::ShaderAsset>("shaders/entity_debug");
    } catch (const std::exception& e) {
        spdlog::warn("Entity debug shader unavailable: {}", e.what());
        return;
    }

    glGenVertexArrays(1, &debug.entityDebug.vao);
    glGenBuffers(1, &debug.entityDebug.vbo);
    glBindVertexArray(debug.entityDebug.vao);
    glBindVertexArray(0);

    debug.entityDebug.locViewProjection = debug.entityDebug.shader->uniform("u_viewProjection");
    debug.entityDebug.locFieldOrigin = debug.entityDebug.shader->uniform("u_fieldOrigin");
    debug.entityDebug.locFieldRight = debug.entityDebug.shader->uniform("u_fieldRight");
    debug.entityDebug.locFieldUp = debug.entityDebug.shader->uniform("u_fieldUp");
    debug.entityDebug.locFieldForward = debug.entityDebug.shader->uniform("u_fieldForward");
    debug.entityDebug.locCellSize = debug.entityDebug.shader->uniform("u_cellSize");
    debug.entityDebug.locColor = debug.entityDebug.shader->uniform("u_color");
    debug.entityDebug.initialized = true;
}

void releaseDebugResources(DebugState& debug) {
    if (debug.field.initialized) {
        if (debug.field.vao != 0) {
            glDeleteVertexArrays(1, &debug.field.vao);
            debug.field.vao = 0;
        }
        for (GLuint& vbo : debug.field.vbos) {
            if (vbo != 0) {
                glDeleteBuffers(1, &vbo);
                vbo = 0;
            }
        }
        debug.field.initialized = false;
    }
    if (debug.frameGraph.initialized) {
        if (debug.frameGraph.vao != 0) {
            glDeleteVertexArrays(1, &debug.frameGraph.vao);
            debug.frameGraph.vao = 0;
        }
        if (debug.frameGraph.vbo != 0) {
            glDeleteBuffers(1, &debug.frameGraph.vbo);
            debug.frameGraph.vbo = 0;
        }
        debug.frameGraph.initialized = false;
    }
    if (debug.entityDebug.initialized) {
        if (debug.entityDebug.vao != 0) {
            glDeleteVertexArrays(1, &debug.entityDebug.vao);
            debug.entityDebug.vao = 0;
        }
        if (debug.entityDebug.vbo != 0) {
            glDeleteBuffers(1, &debug.entityDebug.vbo);
            debug.entityDebug.vbo = 0;
        }
        debug.entityDebug.initialized = false;
    }
}

void recordFrameTime(DebugState& debug, float seconds) {
    if (!debug.frameGraph.initialized || seconds <= 0.0f) {
        return;
    }
    float ms = seconds * 1000.0f;
    debug.frameGraph.samples[debug.frameGraph.cursor] = ms;
    debug.frameGraph.cursor = (debug.frameGraph.cursor + 1) % debug.frameGraph.samples.size();
    debug.frameGraph.filled = std::min(debug.frameGraph.samples.size(), debug.frameGraph.filled + 1);
}

void renderFrameGraph(DebugState& debug) {
    if (!debug.overlayEnabled || !debug.frameGraph.initialized || debug.frameGraph.filled == 0) {
        return;
    }

    const size_t sampleCount = debug.frameGraph.samples.size();
    const float barWidth = 2.0f / static_cast<float>(sampleCount);
    const float baseY = kFrameGraphBottom;
    const float topSpan = kFrameGraphHeight;

    std::vector<glm::vec2> vertices;
    vertices.reserve(debug.frameGraph.filled * 6);

    for (size_t i = 0; i < debug.frameGraph.filled; ++i) {
        size_t sampleIndex = (debug.frameGraph.cursor + sampleCount - 1 - i) % sampleCount;
        float ms = debug.frameGraph.samples[sampleIndex];
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

    debug.frameGraph.shader->bind();
    if (debug.frameGraph.locColor >= 0) {
        glm::vec4 color(0.2f, 0.9f, 0.9f, 0.85f);
        glUniform4fv(debug.frameGraph.locColor, 1, glm::value_ptr(color));
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(debug.frameGraph.vao);
    glBindBuffer(GL_ARRAY_BUFFER, debug.frameGraph.vbo);
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

void renderProfilerOverlay(DebugState& debug, int viewportWidth, int viewportHeight) {
    if (!debug.overlayEnabled || !debug.profilerOverlayEnabled || !debug.frameGraph.initialized) {
        return;
    }

    (void)viewportWidth;
    (void)viewportHeight;

    const Core::ProfilerFrame* frame = Core::Profiler::getLastFrame();
    if (!frame || frame->frameEndNs <= frame->frameStartNs) {
        return;
    }

    std::vector<const Core::ProfilerRecord*> depthOne;
    depthOne.reserve(frame->records.size());
    for (const auto& record : frame->records) {
        if (record.depth == 1 && record.endNs > record.startNs) {
            depthOne.push_back(&record);
        }
    }

    const uint16_t targetDepth = depthOne.empty() ? 0 : 1;
    std::vector<const Core::ProfilerRecord*> records;
    records.reserve(frame->records.size());
    for (const auto& record : frame->records) {
        if (record.depth != targetDepth) {
            continue;
        }
        if (record.endNs <= record.startNs) {
            continue;
        }
        records.push_back(&record);
    }

    if (records.empty()) {
        return;
    }

    std::stable_sort(records.begin(), records.end(),
                     [](const Core::ProfilerRecord* a, const Core::ProfilerRecord* b) {
                         return (a->endNs - a->startNs) > (b->endNs - b->startNs);
                     });

    const uint64_t frameDurationNs = frame->frameEndNs - frame->frameStartNs;
    const float widthSpan = kProfilerRight - kProfilerLeft;
    const std::array<glm::vec4, 6> colors = {
        glm::vec4(0.9f, 0.4f, 0.2f, 0.8f),
        glm::vec4(0.2f, 0.7f, 0.9f, 0.8f),
        glm::vec4(0.4f, 0.9f, 0.3f, 0.8f),
        glm::vec4(0.9f, 0.8f, 0.2f, 0.8f),
        glm::vec4(0.7f, 0.4f, 0.9f, 0.8f),
        glm::vec4(0.9f, 0.3f, 0.6f, 0.8f)
    };

    debug.frameGraph.shader->bind();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(debug.frameGraph.vao);
    glBindBuffer(GL_ARRAY_BUFFER, debug.frameGraph.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);

    const size_t barCount = std::min(records.size(), static_cast<size_t>(kProfilerMaxBars));
    for (size_t i = 0; i < barCount; ++i) {
        const Core::ProfilerRecord* record = records[i];
        uint64_t duration = record->endNs - record->startNs;
        float t = frameDurationNs > 0
            ? static_cast<float>(duration) / static_cast<float>(frameDurationNs)
            : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);

        float x0 = kProfilerLeft;
        float x1 = kProfilerLeft + widthSpan * t;
        float y0 = kProfilerBottom + static_cast<float>(i) * (kProfilerBarHeight + kProfilerBarGap);
        float y1 = y0 + kProfilerBarHeight;

        glm::vec2 vertices[] = {
            {x0, y0},
            {x1, y0},
            {x1, y1},
            {x0, y0},
            {x1, y1},
            {x0, y1}
        };

        if (debug.frameGraph.locColor >= 0) {
            const glm::vec4& color = colors[i % colors.size()];
            glUniform4fv(debug.frameGraph.locColor, 1, glm::value_ptr(color));
        }

        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sizeof(vertices)),
                     vertices,
                     GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
}

void renderDebugField(DebugState& debug,
                      const Voxel::WorldView* worldView,
                      const glm::vec3& cameraPos,
                      const glm::vec3& cameraTarget,
                      const glm::vec3& viewForward,
                      int viewportWidth,
                      int viewportHeight) {
    if (!debug.overlayEnabled || !debug.field.initialized || !worldView) {
        return;
    }

    worldView->getChunkDebugStates(debug.debugStates);
    if (debug.debugStates.empty()) {
        return;
    }

    int radius = std::max(0, worldView->viewDistanceChunks());
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
    stateMap.reserve(debug.debugStates.size());
    std::array<std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash>, 4> occupancy;
    for (auto& set : occupancy) {
        set.reserve(debug.debugStates.size());
    }

    for (const auto& entry : debug.debugStates) {
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

    float renderDistance = worldView->renderConfig().renderDistance;
    float farPlane = std::max(500.0f, renderDistance + static_cast<float>(Voxel::Chunk::SIZE));
    glm::mat4 debugProjection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, farPlane);
    glm::mat4 debugView = glm::lookAt(
        cameraPos,
        cameraTarget,
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 debugViewProjection = debugProjection * debugView;

    debug.field.shader->bind();
    if (debug.field.locViewProjection >= 0) {
        glUniformMatrix4fv(debug.field.locViewProjection, 1, GL_FALSE, glm::value_ptr(debugViewProjection));
    }
    glm::vec3 fieldOrigin = cameraPos + viewForward * debug.debugDistance;
    if (debug.field.locFieldOrigin >= 0) {
        glUniform3fv(debug.field.locFieldOrigin, 1, glm::value_ptr(fieldOrigin));
    }
    if (debug.field.locFieldRight >= 0) {
        glUniform3fv(debug.field.locFieldRight, 1, glm::value_ptr(glm::vec3(1.0f, 0.0f, 0.0f)));
    }
    if (debug.field.locFieldUp >= 0) {
        glUniform3fv(debug.field.locFieldUp, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    if (debug.field.locFieldForward >= 0) {
        glUniform3fv(debug.field.locFieldForward, 1, glm::value_ptr(glm::vec3(0.0f, 0.0f, 1.0f)));
    }
    if (debug.field.locCellSize >= 0) {
        glUniform1f(debug.field.locCellSize, cellSize);
    }

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(debug.field.vao);
    for (size_t i = 0; i < meshVertices.size(); ++i) {
        if (meshVertices[i].empty()) {
            continue;
        }

        if (debug.field.locColor >= 0) {
            glUniform4fv(debug.field.locColor, 1, glm::value_ptr(colors[i]));
        }

        glBindBuffer(GL_ARRAY_BUFFER, debug.field.vbos[i]);
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

void renderEntityDebugBoxes(DebugState& debug,
                            const Voxel::World* world,
                            const glm::mat4& view,
                            const glm::mat4& projection) {
    if (!debug.overlayEnabled || !debug.entityDebug.initialized || !world || world->entities().size() == 0) {
        return;
    }

    constexpr size_t kCubeVertexCount = sizeof(kCubeVertices) / (sizeof(float) * 3);
    std::vector<glm::vec3> vertices;
    vertices.reserve(world->entities().size() * kCubeVertexCount);

    world->entities().forEach([&](const Entity::Entity& entity) {
        const Entity::Aabb& bounds = entity.worldBounds();
        glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
        glm::vec3 size = bounds.max - bounds.min;

        for (size_t i = 0; i < kCubeVertexCount; ++i) {
            size_t base = i * 3;
            glm::vec3 local(kCubeVertices[base + 0],
                            kCubeVertices[base + 1],
                            kCubeVertices[base + 2]);
            vertices.push_back(center + local * size);
        }
    });

    if (vertices.empty()) {
        return;
    }

    glm::mat4 viewProjection = projection * view;

    debug.entityDebug.shader->bind();
    if (debug.entityDebug.locViewProjection >= 0) {
        glUniformMatrix4fv(debug.entityDebug.locViewProjection, 1, GL_FALSE,
                           glm::value_ptr(viewProjection));
    }
    if (debug.entityDebug.locFieldOrigin >= 0) {
        glUniform3fv(debug.entityDebug.locFieldOrigin, 1, glm::value_ptr(glm::vec3(0.0f)));
    }
    if (debug.entityDebug.locFieldRight >= 0) {
        glUniform3fv(debug.entityDebug.locFieldRight, 1, glm::value_ptr(glm::vec3(1.0f, 0.0f, 0.0f)));
    }
    if (debug.entityDebug.locFieldUp >= 0) {
        glUniform3fv(debug.entityDebug.locFieldUp, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    if (debug.entityDebug.locFieldForward >= 0) {
        glUniform3fv(debug.entityDebug.locFieldForward, 1, glm::value_ptr(glm::vec3(0.0f, 0.0f, 1.0f)));
    }
    if (debug.entityDebug.locCellSize >= 0) {
        glUniform1f(debug.entityDebug.locCellSize, 1.0f);
    }
    if (debug.entityDebug.locColor >= 0) {
        glm::vec4 color(1.0f, 0.1f, 0.1f, 0.9f);
        glUniform4fv(debug.entityDebug.locColor, 1, glm::value_ptr(color));
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    glBindVertexArray(debug.entityDebug.vao);
    glBindBuffer(GL_ARRAY_BUFFER, debug.entityDebug.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec3)),
                 vertices.data(),
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace Rigel::Render
