#pragma once

struct GLFWwindow;

namespace Rigel::UI {

bool init(GLFWwindow* window);
void shutdown();

void beginFrame();
void endFrame();

void renderProfilerWindow(bool enabled);

bool wantsCaptureKeyboard();
bool wantsCaptureMouse();

} // namespace Rigel::UI
