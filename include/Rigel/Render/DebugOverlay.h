#pragma once

#include "Rigel/Asset/Handle.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

#include <array>
#include <cstddef>
#include <vector>

#include <GL/glew.h>
#include <glm/glm.hpp>

namespace Rigel {
namespace Asset { class AssetManager; class ShaderAsset; }
namespace Voxel { class WorldView; class World; }

namespace Render {

constexpr float kDefaultDebugDistance = 8.0f;
constexpr size_t kChunkDebugStateBuckets = 5;
constexpr size_t kVoxelSvoDebugStateBuckets = 5;
constexpr size_t kDebugStateBuckets = kChunkDebugStateBuckets + kVoxelSvoDebugStateBuckets;

struct DebugField {
    GLuint vao = 0;
    std::array<GLuint, kDebugStateBuckets> vbos{};
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
    std::vector<std::pair<Voxel::VoxelPageKey, Voxel::VoxelSvoPageInfo>> svoDebugPages;
    float debugDistance = kDefaultDebugDistance;
    bool overlayEnabled = true;
    bool imguiEnabled = false;
};

void initDebugField(DebugState& debug, Asset::AssetManager& assets);

void initFrameGraph(DebugState& debug, Asset::AssetManager& assets);

void initEntityDebug(DebugState& debug, Asset::AssetManager& assets);

void releaseDebugResources(DebugState& debug);

void recordFrameTime(DebugState& debug, float seconds);

void renderFrameGraph(DebugState& debug);

void renderDebugField(DebugState& debug,
                      const Voxel::WorldView* worldView,
                      const glm::vec3& cameraPos,
                      const glm::vec3& cameraTarget,
                      const glm::vec3& viewForward,
                      int viewportWidth,
                      int viewportHeight);

void renderEntityDebugBoxes(DebugState& debug,
                            const Voxel::World* world,
                            const glm::mat4& view,
                            const glm::mat4& projection);

} // namespace Render
} // namespace Rigel
