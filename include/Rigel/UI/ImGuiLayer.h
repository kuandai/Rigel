#pragma once

struct GLFWwindow;

namespace Rigel::Render {
struct ChunkDebugDetailPresentation;
}
namespace Rigel::Voxel {
struct BlockGalleryTargetPresentation;
}

namespace Rigel::UI {

bool init(GLFWwindow* window);
void shutdown();

void beginFrame();
void endFrame();

void renderProfilerWindow(bool enabled);
void renderChunkDebugLegend(
    bool enabled,
    const Render::ChunkDebugDetailPresentation* detail);
void renderBlockGalleryTarget(
    const Voxel::BlockGalleryTargetPresentation* target);

bool wantsCaptureKeyboard();
bool wantsCaptureMouse();

} // namespace Rigel::UI
