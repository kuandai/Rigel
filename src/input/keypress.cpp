#include <Rigel/input/keypress.h>
#include <cstdint>

#include <spdlog/spdlog.h>

#if defined(RIGEL_ENABLE_IMGUI)
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#endif

namespace Rigel::Input {

// Bitwise and w/ keystate != 0
const uint8_t PRESSED_BITMASK = 0x01,
              JUST_PRESSED_BITMASK = 0x02,
              JUST_RELEASED_BITMASK = 0x04,
              REPEAT_BITMASK = 0x08;

// Double buffered key states
uint8_t currentKeyStates[GLFW_KEY_LAST + 1] = {0};   // What systems read from
uint8_t pendingKeyStates[GLFW_KEY_LAST + 1] = {0};   // What callbacks write to

bool isKeyPressed(int keycode) {
    if (keycode < 0 || keycode > GLFW_KEY_LAST) return false;
    return (PRESSED_BITMASK & currentKeyStates[keycode]);
}

bool isKeyJustPressed(int keycode) {
    if (keycode < 0 || keycode > GLFW_KEY_LAST) return false;
    return (JUST_PRESSED_BITMASK & currentKeyStates[keycode]);
}

bool isKeyJustReleased(int keycode) {
    if (keycode < 0 || keycode > GLFW_KEY_LAST) return false;
    return (JUST_RELEASED_BITMASK & currentKeyStates[keycode]);
}

bool isKeyRepeating(int keycode) {
    if (keycode < 0 || keycode > GLFW_KEY_LAST) return false;
    return (REPEAT_BITMASK & currentKeyStates[keycode]);
}

void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
#if defined(RIGEL_ENABLE_IMGUI)
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    }
#endif
    if (key < 0 || key > GLFW_KEY_LAST) return;

    if (action == GLFW_PRESS) {
        // Only set just-pressed if wasn't already pressed
        if (!(pendingKeyStates[key] & PRESSED_BITMASK)) {
            pendingKeyStates[key] |= JUST_PRESSED_BITMASK;
        }
        pendingKeyStates[key] |= PRESSED_BITMASK;
    } else if (action == GLFW_RELEASE) {
        pendingKeyStates[key] &= ~PRESSED_BITMASK;        // Clear pressed bit
        pendingKeyStates[key] &= ~REPEAT_BITMASK;         // Clear repeat bit
        pendingKeyStates[key] |= JUST_RELEASED_BITMASK;   // Set just-released bit
    } else if (action == GLFW_REPEAT) {
        pendingKeyStates[key] |= REPEAT_BITMASK;
    }
}

void keyupdate() {    
    // Copy pending state to current state (what systems will read)
    for (int i = 0; i <= GLFW_KEY_LAST; ++i) {
        currentKeyStates[i] = pendingKeyStates[i];
    }
    
    // Clear just-pressed and just-released flags from pending for next frame
    for (int i = 0; i <= GLFW_KEY_LAST; ++i) {
        pendingKeyStates[i] &= (PRESSED_BITMASK | REPEAT_BITMASK);  // Keep pressed and repeat bits
    }
}

} // namespace Rigel::Input
