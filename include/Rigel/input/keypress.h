#pragma once

#include <GLFW/glfw3.h>

namespace Rigel::Input {
bool isKeyPressed(int keycode);
bool isKeyJustPressed(int keycode);
bool isKeyJustReleased(int keycode);
bool isKeyRepeating(int keycode);

// glfwSetKeyCallback
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

// Swaps key buffers, call after GLFWPollEvents and before querying
void keyupdate();

} // namespace Rigel::Input
