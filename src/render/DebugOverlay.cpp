#include "Rigel/Render/DebugOverlay.h"

#include "Rigel/Asset/AssetManager.h"
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

struct SvoDebugCellCoord {
    int x = 0;
    int y = 0;
    int z = 0;
    int span = 1;

    bool operator==(const SvoDebugCellCoord& other) const {
        return x == other.x && y == other.y && z == other.z && span == other.span;
    }
};

struct SvoDebugCellCoordHash {
    size_t operator()(const SvoDebugCellCoord& key) const noexcept {
        size_t h = 2166136261u;
        auto mix = [&h](int value) {
            h ^= static_cast<size_t>(value) + 0x9e3779b9u + (h << 6) + (h >> 2);
        };
        mix(key.x);
        mix(key.y);
        mix(key.z);
        mix(key.span);
        return h;
    }
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
    worldView->getSvoDebugStates(debug.svoDebugStates);
    if (debug.debugStates.empty() && debug.svoDebugStates.empty()) {
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
                       Voxel::ChunkCoordHash> chunkStateMap;
    chunkStateMap.reserve(debug.debugStates.size());
    std::array<std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash>, kChunkDebugStateBuckets>
        chunkOccupancy;
    for (auto& set : chunkOccupancy) {
        set.reserve(debug.debugStates.size());
    }

    std::unordered_map<SvoDebugCellCoord,
                       Voxel::LodCellState,
                       SvoDebugCellCoordHash> svoStateMap;
    svoStateMap.reserve(debug.svoDebugStates.size());
    std::array<std::unordered_set<SvoDebugCellCoord, SvoDebugCellCoordHash>, kSvoDebugStateBuckets>
        svoOccupancy;
    for (auto& set : svoOccupancy) {
        set.reserve(debug.svoDebugStates.size());
    }

    for (const auto& entry : debug.debugStates) {
        int dx = entry.coord.x - centerCoord.x;
        int dy = entry.coord.y - centerCoord.y;
        int dz = entry.coord.z - centerCoord.z;

        if (std::abs(dx) > radius || std::abs(dy) > radius || std::abs(dz) > radius) {
            continue;
        }
        Voxel::ChunkCoord offset{dx, dy, dz};
        chunkStateMap[offset] = entry.state;
        switch (entry.state) {
            case Voxel::ChunkStreamer::DebugState::QueuedGen:
                chunkOccupancy[0].insert(offset);
                break;
            case Voxel::ChunkStreamer::DebugState::LoadedFromDisk:
                chunkOccupancy[1].insert(offset);
                break;
            case Voxel::ChunkStreamer::DebugState::ReadyData:
                chunkOccupancy[2].insert(offset);
                break;
            case Voxel::ChunkStreamer::DebugState::QueuedMesh:
                chunkOccupancy[3].insert(offset);
                break;
            case Voxel::ChunkStreamer::DebugState::ReadyMesh:
                chunkOccupancy[4].insert(offset);
                break;
        }
    }

    for (const auto& entry : debug.svoDebugStates) {
        const int span = std::max(1, entry.spanChunks);
        const int minX = entry.key.x * span - centerCoord.x;
        const int minY = entry.key.y * span - centerCoord.y;
        const int minZ = entry.key.z * span - centerCoord.z;
        const int maxX = minX + span;
        const int maxY = minY + span;
        const int maxZ = minZ + span;
        const int maxView = radius + 1;
        if (maxX < -radius || minX > maxView ||
            maxY < -radius || minY > maxView ||
            maxZ < -radius || minZ > maxView) {
            continue;
        }

        int stateIdx = -1;
        switch (entry.state) {
            case Voxel::LodCellState::QueuedBuild:
                stateIdx = 0;
                break;
            case Voxel::LodCellState::Building:
                stateIdx = 1;
                break;
            case Voxel::LodCellState::Ready:
                stateIdx = 2;
                break;
            case Voxel::LodCellState::Stale:
                stateIdx = 3;
                break;
            case Voxel::LodCellState::Evicting:
                stateIdx = 4;
                break;
            case Voxel::LodCellState::Missing:
                break;
        }
        if (stateIdx < 0) {
            continue;
        }

        SvoDebugCellCoord coord{minX, minY, minZ, span};
        svoStateMap[coord] = entry.state;
        svoOccupancy[static_cast<size_t>(stateIdx)].insert(coord);
    }

    if (chunkStateMap.empty() && svoStateMap.empty()) {
        return;
    }

    std::array<std::vector<glm::vec3>, kDebugStateBuckets> meshVertices;
    std::array<glm::vec4, kDebugStateBuckets> colors = {
        glm::vec4(1.0f, 0.2f, 0.2f, kDebugAlpha),
        glm::vec4(0.8f, 0.8f, 0.8f, kDebugAlpha),
        glm::vec4(1.0f, 0.9f, 0.2f, kDebugAlpha),
        glm::vec4(0.2f, 0.8f, 1.0f, kDebugAlpha),
        glm::vec4(0.2f, 1.0f, 0.3f, kDebugAlpha),
        glm::vec4(0.95f, 0.35f, 1.0f, 0.22f),
        glm::vec4(1.0f, 0.55f, 0.2f, 0.22f),
        glm::vec4(0.35f, 1.0f, 0.55f, 0.22f),
        glm::vec4(1.0f, 0.4f, 0.2f, 0.22f),
        glm::vec4(0.7f, 0.4f, 1.0f, 0.22f)
    };
    std::array<std::array<int, 3>, 6> offsets = {{
        { 1, 0, 0},
        {-1, 0, 0},
        { 0, 1, 0},
        { 0,-1, 0},
        { 0, 0, 1},
        { 0, 0,-1}
    }};

    for (const auto& [coord, state] : chunkStateMap) {
        int stateIdx = 0;
        switch (state) {
            case Voxel::ChunkStreamer::DebugState::QueuedGen:
                stateIdx = 0;
                break;
            case Voxel::ChunkStreamer::DebugState::LoadedFromDisk:
                stateIdx = 1;
                break;
            case Voxel::ChunkStreamer::DebugState::ReadyData:
                stateIdx = 2;
                break;
            case Voxel::ChunkStreamer::DebugState::QueuedMesh:
                stateIdx = 3;
                break;
            case Voxel::ChunkStreamer::DebugState::ReadyMesh:
                stateIdx = 4;
                break;
        }

        for (int face = 0; face < 6; ++face) {
            Voxel::ChunkCoord neighbor{
                coord.x + offsets[face][0],
                coord.y + offsets[face][1],
                coord.z + offsets[face][2]
            };
            if (chunkOccupancy[static_cast<size_t>(stateIdx)].find(neighbor) !=
                chunkOccupancy[static_cast<size_t>(stateIdx)].end()) {
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

    for (const auto& [coord, state] : svoStateMap) {
        int stateIdx = -1;
        switch (state) {
            case Voxel::LodCellState::QueuedBuild:
                stateIdx = static_cast<int>(kChunkDebugStateBuckets) + 0;
                break;
            case Voxel::LodCellState::Building:
                stateIdx = static_cast<int>(kChunkDebugStateBuckets) + 1;
                break;
            case Voxel::LodCellState::Ready:
                stateIdx = static_cast<int>(kChunkDebugStateBuckets) + 2;
                break;
            case Voxel::LodCellState::Stale:
                stateIdx = static_cast<int>(kChunkDebugStateBuckets) + 3;
                break;
            case Voxel::LodCellState::Evicting:
                stateIdx = static_cast<int>(kChunkDebugStateBuckets) + 4;
                break;
            case Voxel::LodCellState::Missing:
                break;
        }
        if (stateIdx < static_cast<int>(kChunkDebugStateBuckets)) {
            continue;
        }

        const int span = std::max(1, coord.span);
        const float spanScale = static_cast<float>(span);
        const float centerX = static_cast<float>(coord.x) + (spanScale * 0.5f);
        const float centerY = static_cast<float>(coord.y) + (spanScale * 0.5f);
        const float centerZ = static_cast<float>(coord.z) + (spanScale * 0.5f);
        const size_t svoBucket = static_cast<size_t>(stateIdx - static_cast<int>(kChunkDebugStateBuckets));

        for (int face = 0; face < 6; ++face) {
            SvoDebugCellCoord neighbor{
                coord.x + offsets[face][0] * span,
                coord.y + offsets[face][1] * span,
                coord.z + offsets[face][2] * span,
                span
            };
            if (svoOccupancy[svoBucket].find(neighbor) != svoOccupancy[svoBucket].end()) {
                continue;
            }

            size_t base = static_cast<size_t>(face) * 18;
            for (int v = 0; v < 6; ++v) {
                float x = kCubeVertices[base + v * 3 + 0] * spanScale + centerX;
                float y = kCubeVertices[base + v * 3 + 1] * spanScale + centerY;
                float z = kCubeVertices[base + v * 3 + 2] * spanScale + centerZ;
                meshVertices[static_cast<size_t>(stateIdx)].push_back(glm::vec3(x, y, z));
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
