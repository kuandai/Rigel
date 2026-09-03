#pragma once

#include "Rigel/Asset/Handle.h"
#include "Rigel/Entity/Aabb.h"
#include "Rigel/Render/ChunkDebugPresentation.h"
#include "Rigel/Voxel/ChunkStreamer.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <GL/glew.h>
#include <glm/glm.hpp>

namespace Rigel {
namespace Asset { class AssetManager; class ShaderAsset; }
namespace Voxel {
class BlockRegistry;
class WorldView;
class World;
struct BlockTarget;
}

namespace Render {

constexpr float kDefaultDebugDistance = 8.0f;

struct DebugField {
    GLuint vao = 0;
    std::array<GLuint, kChunkDebugPresentationCount> vbos{};
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

struct EntityDebug {
    GLuint vao = 0;
    GLuint vbo = 0;
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

struct DebugState {
    DebugField field;
    FrameTimeGraph frameGraph;
    EntityDebug entityDebug;
    std::vector<Voxel::ChunkStreamer::DebugChunkState> debugStates;
    std::optional<ChunkDebugDetailPresentation> chunkDetail;
    float debugDistance = kDefaultDebugDistance;
    bool overlayEnabled = false;
    bool imguiEnabled = false;
};

// CPU presentation produced by the production debug-field path before any GL
// calls. Keeping this boundary explicit allows renderer-independent timing on
// hosts without a graphics context.
struct DebugFieldPresentation {
    std::array<
        std::vector<glm::vec3>,
        kChunkDebugPresentationCount> meshVertices;
    float cellSize = 0.0f;
};

using AabbEdgeVertices = std::array<glm::vec3, 24>;

/** Build the twelve non-diagonal edges of a world-space axis-aligned box. */
AabbEdgeVertices makeAabbEdgeVertices(
    const Entity::Aabb& bounds,
    const glm::vec3& translation = glm::vec3{0.0f},
    float expansion = 0.0f);

/** Compatibility overload for callers that already hold separate bounds. */
AabbEdgeVertices makeAabbEdgeVertices(
    const glm::vec3& minimum,
    const glm::vec3& maximum);

/**
 * Build tightly packed GL_LINES vertices for any number of world-space boxes.
 * Translation and non-negative expansion are applied to every input box.
 */
std::vector<glm::vec3> buildAabbEdgeLinePresentation(
    std::span<const Entity::Aabb> boxes,
    const glm::vec3& translation = glm::vec3{0.0f},
    float expansion = 0.0f);

void initDebugField(DebugState& debug, Asset::AssetManager& assets);

void initFrameGraph(DebugState& debug, Asset::AssetManager& assets);

void initEntityDebug(DebugState& debug, Asset::AssetManager& assets);

void releaseDebugResources(DebugState& debug);

void recordFrameTime(DebugState& debug, float seconds);

void renderFrameGraph(DebugState& debug);

std::optional<DebugFieldPresentation> buildDebugFieldPresentation(
    DebugState& debug,
    const Voxel::WorldView* worldView,
    const glm::vec3& cameraPos);

void renderDebugField(DebugState& debug,
                      const Voxel::WorldView* worldView,
                      const glm::vec3& cameraPos,
                      const glm::vec3& cameraTarget,
                      const glm::vec3& viewForward,
                      int viewportWidth,
                      int viewportHeight,
                      const glm::mat4& projection);

void renderEntityDebugBoxes(DebugState& debug,
                            const Voxel::World* world,
                            const glm::mat4& view,
                            const glm::mat4& projection);

void renderBlockTargetOutline(
    DebugState& debug,
    const Voxel::BlockRegistry& registry,
    const Voxel::BlockTarget* target,
    const glm::mat4& view,
    const glm::mat4& projection);

} // namespace Render
} // namespace Rigel
