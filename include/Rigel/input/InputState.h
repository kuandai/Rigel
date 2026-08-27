#pragma once

#include "Rigel/input/InputBindings.h"

#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Input {

class InputListener {
public:
    virtual ~InputListener() = default;

    virtual void onActionPressed(std::string_view action) {
        (void)action;
    }

    virtual void onActionReleased(std::string_view action) {
        (void)action;
    }
};

class InputState {
public:
    void setBindings(std::shared_ptr<const InputBindings> bindings);
    void addListener(InputListener* listener);
    void removeListener(InputListener* listener);

    void handleKeyEvent(int key, int action);
    void handleMouseButtonEvent(int button, int action);
    void handleFocusLost();
    void beginFrame();

    bool isKeyPressed(int key) const;
    bool isKeyJustPressed(int key) const;
    bool isKeyJustReleased(int key) const;
    bool isKeyRepeating(int key) const;

    bool isMouseButtonJustPressed(int button) const;

    bool isActionPressed(std::string_view action) const;
    bool isActionJustPressed(std::string_view action) const;
    bool isActionJustReleased(std::string_view action) const;

private:
    using KeyStates = std::array<std::uint8_t, GLFW_KEY_LAST + 1>;
    using MouseButtonStates = std::array<std::uint8_t, GLFW_MOUSE_BUTTON_LAST + 1>;

    struct PendingBindings {
        std::shared_ptr<const InputBindings> bindings;
        KeyStates keysAtQueue{};
        MouseButtonStates mouseButtonsAtQueue{};
    };

    bool isPhysicalInputPressed(
        const PhysicalInput& input,
        const KeyStates& keys,
        const MouseButtonStates& mouseButtons) const;
    void rebaseActionStates(
        const KeyStates& keys,
        const MouseButtonStates& mouseButtons);
    void updateActionTransitions();
    void dispatchActionTransitions();

    KeyStates m_currentKeys{};
    KeyStates m_pendingKeys{};
    MouseButtonStates m_currentMouseButtons{};
    MouseButtonStates m_pendingMouseButtons{};
    std::shared_ptr<const InputBindings> m_bindings;
    std::optional<PendingBindings> m_pendingBindings;
    std::unordered_map<std::string, std::uint8_t> m_actionStates;
    std::vector<InputListener*> m_listeners;
};

} // namespace Rigel::Input
