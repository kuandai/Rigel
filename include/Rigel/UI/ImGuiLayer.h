#pragma once

struct GLFWwindow;

namespace Rigel::Voxel {
struct VoxelSvoConfig;
struct VoxelSvoTelemetry;
}

namespace Rigel::UI {

bool init(GLFWwindow* window);
void shutdown();

void beginFrame();
void endFrame();

void renderProfilerWindow(bool enabled,
                          const Rigel::Voxel::VoxelSvoConfig* voxelSvoConfig = nullptr,
                          const Rigel::Voxel::VoxelSvoTelemetry* voxelSvoTelemetry = nullptr);

bool wantsCaptureKeyboard();
bool wantsCaptureMouse();

} // namespace Rigel::UI
