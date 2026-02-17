#pragma once

struct GLFWwindow;

namespace Rigel::Voxel {
struct SvoLodConfig;
struct SvoLodTelemetry;
}

namespace Rigel::UI {

bool init(GLFWwindow* window);
void shutdown();

void beginFrame();
void endFrame();

void renderProfilerWindow(bool enabled,
                          const Rigel::Voxel::SvoLodConfig* svoConfig = nullptr,
                          const Rigel::Voxel::SvoLodTelemetry* svoTelemetry = nullptr);

bool wantsCaptureKeyboard();
bool wantsCaptureMouse();

} // namespace Rigel::UI
