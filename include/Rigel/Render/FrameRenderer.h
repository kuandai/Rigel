#pragma once

#include <memory>

#include <glm/vec3.hpp>

namespace Rigel {
namespace Asset { class AssetManager; }
namespace Voxel { class World; class WorldView; struct BlockTarget; }

namespace Render {

struct ChunkDebugDetailPresentation;
class FrameRendererTestAccess;

struct FrameRenderContext {
    Voxel::World& world;
    Voxel::WorldView& worldView;
    glm::vec3 cameraPosition;
    glm::vec3 cameraTarget;
    glm::vec3 cameraForward;
    int viewportWidth = 0;
    int viewportHeight = 0;
    float deltaTime = 0.0f;
    const Voxel::BlockTarget* blockTarget = nullptr;
};

class FrameRenderer {
public:
    FrameRenderer();
    ~FrameRenderer();

    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;

    void initialize(Asset::AssetManager& assets);
    void release();

    void recordFrameTime(float seconds);
    void setVerticalFovDegrees(double verticalFovDegrees);
    void render(const FrameRenderContext& context);
    void clear(int viewportWidth, int viewportHeight);

    bool& debugOverlayEnabled();
    bool& profilerWindowEnabled();
    const ChunkDebugDetailPresentation* chunkDebugDetail() const;

private:
    friend class FrameRendererTestAccess;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Render
} // namespace Rigel
