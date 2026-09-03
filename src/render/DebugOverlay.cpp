#include "Rigel/Render/DebugOverlay.h"

#include "../voxel/BlockModelGeometry.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldView.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
// Keep outline edges just outside coplanar model surfaces to avoid depth
// fighting while retaining normal depth occlusion by nearer geometry.
constexpr float kBlockTargetOutlineExpansion = 0.002f;
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

std::string_view pipelineOwnerName(
    Voxel::ChunkStreamer::DebugPipelineOwner owner) {
    using Owner = Voxel::ChunkStreamer::DebugPipelineOwner;
    switch (owner) {
        case Owner::WaitingForData: return "waiting_for_data";
        case Owner::WaitingForNeighbors: return "waiting_for_neighbors";
        case Owner::MeshScheduler: return "mesh_scheduler";
        case Owner::MeshWork: return "mesh_work";
        case Owner::DirtyRemesh: return "dirty_remesh";
        case Owner::Complete: return "complete";
        case Owner::TerminalFailure: return "terminal_failure";
    }
    return "unknown";
}

std::string_view voxelOccupancyName(
    Voxel::ChunkStreamer::DebugVoxelOccupancy occupancy) {
    using Occupancy = Voxel::ChunkStreamer::DebugVoxelOccupancy;
    switch (occupancy) {
        case Occupancy::Unknown: return "unknown";
        case Occupancy::Empty: return "empty";
        case Occupancy::Nonempty: return "nonempty";
    }
    return "unknown";
}

std::string_view installedGeometryName(
    Voxel::ChunkStreamer::DebugInstalledGeometry geometry) {
    using Geometry = Voxel::ChunkStreamer::DebugInstalledGeometry;
    switch (geometry) {
        case Geometry::None: return "none";
        case Geometry::Empty: return "empty";
        case Geometry::Nonempty: return "nonempty";
    }
    return "unknown";
}

std::string_view remeshIntentName(
    Voxel::ChunkStreamer::DebugRemeshIntent intent) {
    using Intent = Voxel::ChunkStreamer::DebugRemeshIntent;
    switch (intent) {
        case Intent::None: return "none";
        case Intent::Pending: return "pending";
    }
    return "unknown";
}

std::string_view failureName(Voxel::ChunkStreamer::DebugFailure failure) {
    using Failure = Voxel::ChunkStreamer::DebugFailure;
    switch (failure) {
        case Failure::None: return "none";
        case Failure::Load: return "load";
        case Failure::Generation: return "generation";
        case Failure::Mesh: return "mesh";
        case Failure::Eviction: return "eviction";
    }
    return "unknown";
}

std::string_view drawEvidenceName(
    Voxel::ChunkStreamer::DebugDrawEvidence evidence) {
    using Evidence = Voxel::ChunkStreamer::DebugDrawEvidence;
    switch (evidence) {
        case Evidence::NotApplicable: return "not_applicable";
        case Evidence::NotDrawn: return "not_drawn";
        case Evidence::Drawn: return "drawn";
    }
    return "unknown";
}

std::string historicalTraceKeyName(
    const std::optional<Voxel::ChunkVisibilityLifecycleKey>& key) {
    if (!key) {
        return "none";
    }
    return "(" + std::to_string(key->coord.x) + ", " +
        std::to_string(key->coord.y) + ", " +
        std::to_string(key->coord.z) + ") #" +
        std::to_string(key->lifecycleId);
}

} // namespace

AabbEdgeVertices makeAabbEdgeVertices(
    const Entity::Aabb& bounds,
    const glm::vec3& translation,
    float expansion
) {
    const glm::vec3 expansionVector(expansion);
    const glm::vec3 minimum =
        bounds.min + translation - expansionVector;
    const glm::vec3 maximum =
        bounds.max + translation + expansionVector;
    const std::array corners = {
        glm::vec3{minimum.x, minimum.y, minimum.z},
        glm::vec3{maximum.x, minimum.y, minimum.z},
        glm::vec3{maximum.x, maximum.y, minimum.z},
        glm::vec3{minimum.x, maximum.y, minimum.z},
        glm::vec3{minimum.x, minimum.y, maximum.z},
        glm::vec3{maximum.x, minimum.y, maximum.z},
        glm::vec3{maximum.x, maximum.y, maximum.z},
        glm::vec3{minimum.x, maximum.y, maximum.z},
    };
    constexpr std::array<std::array<size_t, 2>, 12> edges = {{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    }};

    AabbEdgeVertices vertices;
    size_t vertexIndex = 0;
    for (const auto& edge : edges) {
        vertices[vertexIndex++] = corners[edge[0]];
        vertices[vertexIndex++] = corners[edge[1]];
    }
    return vertices;
}

AabbEdgeVertices makeAabbEdgeVertices(
    const glm::vec3& minimum,
    const glm::vec3& maximum
) {
    return makeAabbEdgeVertices(Entity::Aabb{minimum, maximum});
}

std::vector<glm::vec3> buildAabbEdgeLinePresentation(
    std::span<const Entity::Aabb> boxes,
    const glm::vec3& translation,
    float expansion
) {
    std::vector<glm::vec3> vertices;
    vertices.reserve(boxes.size() * AabbEdgeVertices{}.size());
    for (const Entity::Aabb& box : boxes) {
        const AabbEdgeVertices edges =
            makeAabbEdgeVertices(box, translation, expansion);
        vertices.insert(vertices.end(), edges.begin(), edges.end());
    }
    return vertices;
}

std::vector<glm::vec3> buildBlockTargetOutlinePresentation(
    const Voxel::BlockRegistry& registry,
    const Voxel::BlockTarget* target
) {
    if (!target || target->state.id.type >= registry.size()) {
        return {};
    }

    const Voxel::BlockModelInstance& instance =
        registry.getType(target->state.id).model;
    if (!instance || instance->isEmpty() ||
        target->cuboidIndex >= instance->cuboids().size()) {
        return {};
    }

    std::vector<glm::vec3> vertices;
    vertices.reserve(
        instance->cuboids().size() * AabbEdgeVertices{}.size());
    const glm::vec3 translation(target->block);
    for (const Voxel::BlockModelCuboid& cuboid : instance->cuboids()) {
        const Voxel::BlockModelBounds bounds =
            Voxel::detail::orientedBounds(
                cuboid.bounds, instance.orientation);
        const AabbEdgeVertices edges = makeAabbEdgeVertices(
            Entity::Aabb{
                glm::vec3{bounds.min[0], bounds.min[1], bounds.min[2]},
                glm::vec3{bounds.max[0], bounds.max[1], bounds.max[2]}},
            translation,
            kBlockTargetOutlineExpansion);
        vertices.insert(vertices.end(), edges.begin(), edges.end());
    }
    return vertices;
}

namespace {

void restoreCapability(GLenum capability, GLboolean enabled) {
    if (enabled == GL_TRUE) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

class WorldLineGlState final {
public:
    WorldLineGlState()
        : m_depthTest(glIsEnabled(GL_DEPTH_TEST))
        , m_cullFace(glIsEnabled(GL_CULL_FACE))
        , m_blend(glIsEnabled(GL_BLEND)) {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthWriteMask);
        glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vertexArray);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
    }

    ~WorldLineGlState() {
        restoreCapability(GL_DEPTH_TEST, m_depthTest);
        glDepthMask(m_depthWriteMask);
        restoreCapability(GL_CULL_FACE, m_cullFace);
        restoreCapability(GL_BLEND, m_blend);
        glBindVertexArray(static_cast<GLuint>(m_vertexArray));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_arrayBuffer));
        glUseProgram(static_cast<GLuint>(m_program));
    }

    WorldLineGlState(const WorldLineGlState&) = delete;
    WorldLineGlState& operator=(const WorldLineGlState&) = delete;

private:
    GLboolean m_depthTest = GL_FALSE;
    GLboolean m_depthWriteMask = GL_FALSE;
    GLboolean m_cullFace = GL_FALSE;
    GLboolean m_blend = GL_FALSE;
    GLint m_program = 0;
    GLint m_vertexArray = 0;
    GLint m_arrayBuffer = 0;
};

void renderWorldLines(
    EntityDebug& lines,
    std::span<const glm::vec3> vertices,
    const glm::mat4& viewProjection,
    const glm::vec4& color
) {
    if (!lines.initialized || vertices.empty()) {
        return;
    }

    const WorldLineGlState callerState;
    lines.shader->bind();
    if (lines.locViewProjection >= 0) {
        glUniformMatrix4fv(
            lines.locViewProjection,
            1,
            GL_FALSE,
            glm::value_ptr(viewProjection));
    }
    const glm::vec3 origin(0.0f);
    const glm::vec3 right(1.0f, 0.0f, 0.0f);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 forward(0.0f, 0.0f, 1.0f);
    if (lines.locFieldOrigin >= 0) {
        glUniform3fv(lines.locFieldOrigin, 1, glm::value_ptr(origin));
    }
    if (lines.locFieldRight >= 0) {
        glUniform3fv(lines.locFieldRight, 1, glm::value_ptr(right));
    }
    if (lines.locFieldUp >= 0) {
        glUniform3fv(lines.locFieldUp, 1, glm::value_ptr(up));
    }
    if (lines.locFieldForward >= 0) {
        glUniform3fv(lines.locFieldForward, 1, glm::value_ptr(forward));
    }
    if (lines.locCellSize >= 0) {
        glUniform1f(lines.locCellSize, 1.0f);
    }
    if (lines.locColor >= 0) {
        glUniform4fv(lines.locColor, 1, glm::value_ptr(color));
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glBindVertexArray(lines.vao);
    glBindBuffer(GL_ARRAY_BUFFER, lines.vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(vertices.size_bytes()),
        vertices.data(),
        GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices.size()));
}

} // namespace

std::optional<ChunkDebugDetailPresentation> selectChunkDebugDetail(
    std::span<const Voxel::ChunkStreamer::DebugChunkState> states,
    Voxel::ChunkCoord center) {
    if (states.empty()) {
        return std::nullopt;
    }

    const auto* selected = &states.front();
    uint64_t selectedDistance = std::numeric_limits<uint64_t>::max();
    for (const auto& state : states) {
        if (state.historicalTraceKey) {
            selected = &state;
            break;
        }
        const auto coordinateDistance = [](int lhs, int rhs) {
            const int64_t delta =
                static_cast<int64_t>(lhs) - static_cast<int64_t>(rhs);
            return static_cast<uint64_t>(delta < 0 ? -delta : delta);
        };
        const uint64_t distance =
            coordinateDistance(state.coord.x, center.x) +
            coordinateDistance(state.coord.y, center.y) +
            coordinateDistance(state.coord.z, center.z);
        if (distance < selectedDistance) {
            selected = &state;
            selectedDistance = distance;
        }
    }

    const auto presentationIndex = chunkDebugPresentationIndex(selected->state);
    const std::string_view stateName = presentationIndex
        ? kChunkDebugPresentations[*presentationIndex].legend
        : "Unknown state";
    return ChunkDebugDetailPresentation{
        selected->coord,
        {{
            {"Primary state", stateName},
            {"Pipeline owner", pipelineOwnerName(selected->pipelineOwner)},
            {"Voxel occupancy", voxelOccupancyName(selected->voxelOccupancy)},
            {"Installed CPU geometry",
             installedGeometryName(selected->installedGeometry)},
            {"Remesh intent", remeshIntentName(selected->remeshIntent)},
            {"Failure", failureName(selected->failure)},
            {"Historical trace key",
             historicalTraceKeyName(selected->historicalTraceKey)},
            {"Historical trace kind", selected->historicalTraceKind
                ? Voxel::chunkVisibilityLifecycleKindName(
                    *selected->historicalTraceKind)
                : std::string_view{"none"}},
            {"Historical trace outcome", selected->historicalTraceOutcome
                ? Voxel::chunkVisibilityOutcomeName(
                    *selected->historicalTraceOutcome)
                : std::string_view{"none"}},
            {"Historical trace draw outcome",
             selected->historicalTraceDrawOutcome
                ? Voxel::chunkVisibilityDrawOutcomeName(
                    *selected->historicalTraceDrawOutcome)
                : std::string_view{"none"}},
            {"Main-pass draw evidence",
             drawEvidenceName(selected->drawEvidence)}
        }}
    };
}

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

    if (debug.frameGraph.vao != 0) {
        glDeleteVertexArrays(1, &debug.frameGraph.vao);
        debug.frameGraph.vao = 0;
    }
    if (debug.frameGraph.vbo != 0) {
        glDeleteBuffers(1, &debug.frameGraph.vbo);
        debug.frameGraph.vbo = 0;
    }
    debug.frameGraph.initialized = false;

    if (debug.entityDebug.vao != 0) {
        glDeleteVertexArrays(1, &debug.entityDebug.vao);
        debug.entityDebug.vao = 0;
    }
    if (debug.entityDebug.vbo != 0) {
        glDeleteBuffers(1, &debug.entityDebug.vbo);
        debug.entityDebug.vbo = 0;
    }
    debug.entityDebug.initialized = false;
    debug.debugStates.clear();
    debug.chunkDetail.reset();
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

std::optional<DebugFieldPresentation> buildDebugFieldPresentation(
    DebugState& debug,
    const Voxel::WorldView* worldView,
    const glm::vec3& cameraPos) {
    debug.chunkDetail.reset();
    if (!debug.overlayEnabled || !debug.field.initialized || !worldView) {
        debug.debugStates.clear();
        return std::nullopt;
    }

    int radius = std::clamp(
        worldView->viewDistanceChunks(),
        0,
        Preferences::kMaximumViewDistanceChunks);
    int diameter = radius * 2 + 1;
    if (diameter <= 0) {
        return std::nullopt;
    }

    DebugFieldPresentation presentation;
    presentation.cellSize =
        kDebugTargetSpan / static_cast<float>(diameter);

    auto centerCoord = Voxel::worldToChunk(
        static_cast<int>(std::floor(cameraPos.x)),
        static_cast<int>(std::floor(cameraPos.y)),
        static_cast<int>(std::floor(cameraPos.z))
    );

    worldView->getChunkDebugStates(
        debug.debugStates, centerCoord, radius);
    debug.chunkDetail = selectChunkDebugDetail(debug.debugStates, centerCoord);
    if (debug.debugStates.empty()) {
        return std::nullopt;
    }

    std::unordered_map<Voxel::ChunkCoord,
                       Voxel::ChunkStreamer::DebugState,
                       Voxel::ChunkCoordHash> stateMap;
    stateMap.reserve(debug.debugStates.size());
    std::array<
        std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash>,
        kChunkDebugPresentationCount> occupancy;
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
        const auto stateIndex = chunkDebugPresentationIndex(entry.state);
        if (stateIndex) {
            occupancy[*stateIndex].insert(offset);
        }
    }

    if (stateMap.empty()) {
        return std::nullopt;
    }

    std::array<std::array<int, 3>, 6> offsets = {{
        { 1, 0, 0},
        {-1, 0, 0},
        { 0, 1, 0},
        { 0,-1, 0},
        { 0, 0, 1},
        { 0, 0,-1}
    }};

    for (const auto& [coord, state] : stateMap) {
        const auto stateIndex = chunkDebugPresentationIndex(state);
        if (!stateIndex) {
            continue;
        }
        const size_t stateIdx = *stateIndex;

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
                presentation.meshVertices[stateIdx].push_back(
                    glm::vec3(x, y, z));
            }
        }
    }

    return presentation;
}

void renderDebugField(DebugState& debug,
                      const Voxel::WorldView* worldView,
                      const glm::vec3& cameraPos,
                      const glm::vec3& cameraTarget,
                      const glm::vec3& viewForward,
                      int viewportWidth,
                      int viewportHeight,
                      const glm::mat4& projection) {
    auto presentation =
        buildDebugFieldPresentation(debug, worldView, cameraPos);
    if (!presentation) {
        return;
    }
    const float cellSize = presentation->cellSize;
    const auto& meshVertices = presentation->meshVertices;

    GLint previousViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    int viewportSize = std::min(kDebugViewportSize, std::min(viewportWidth, viewportHeight));
    int marginX = std::min(kDebugViewportMargin, std::max(0, viewportWidth - viewportSize));
    int marginY = std::min(kDebugViewportMargin, std::max(0, viewportHeight - viewportSize));
    int viewportX = marginX;
    int viewportY = std::max(0, viewportHeight - viewportSize - marginY);
    glViewport(viewportX, viewportY, viewportSize, viewportSize);

    glm::mat4 debugView = glm::lookAt(
        cameraPos,
        cameraTarget,
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 debugViewProjection = projection * debugView;

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
            const auto& rgb = kChunkDebugPresentations[i].color;
            const glm::vec4 color(rgb[0], rgb[1], rgb[2], kDebugAlpha);
            glUniform4fv(
                debug.field.locColor, 1, glm::value_ptr(color));
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

    std::vector<Entity::Aabb> boxes;
    boxes.reserve(world->entities().size());

    world->entities().forEach([&](const Entity::Entity& entity) {
        boxes.push_back(entity.worldBounds());
    });
    const std::vector<glm::vec3> vertices =
        buildAabbEdgeLinePresentation(boxes);

    renderWorldLines(
        debug.entityDebug,
        vertices,
        projection * view,
        glm::vec4(1.0f, 0.1f, 0.1f, 0.9f));
}

void renderBlockTargetOutline(
    DebugState& debug,
    const Voxel::BlockRegistry& registry,
    const Voxel::BlockTarget* target,
    const glm::mat4& view,
    const glm::mat4& projection
) {
    if (!debug.entityDebug.initialized) {
        return;
    }
    const std::vector<glm::vec3> vertices =
        buildBlockTargetOutlinePresentation(registry, target);
    if (vertices.empty()) {
        return;
    }
    renderWorldLines(
        debug.entityDebug,
        vertices,
        projection * view,
        glm::vec4(0.05f, 0.05f, 0.05f, 1.0f));
}

} // namespace Rigel::Render
