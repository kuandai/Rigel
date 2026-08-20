#include "Rigel/input/InputState.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Rigel::Input {
namespace {

constexpr std::uint8_t kPressed = 0x01;
constexpr std::uint8_t kJustPressed = 0x02;
constexpr std::uint8_t kJustReleased = 0x04;
constexpr std::uint8_t kRepeating = 0x08;

void applyButtonEvent(std::uint8_t& state, int action) {
    if (action == GLFW_PRESS) {
        if (!(state & kPressed)) {
            state |= kJustPressed;
        }
        state |= kPressed;
    } else if (action == GLFW_RELEASE) {
        state &= static_cast<std::uint8_t>(~kPressed);
        state &= static_cast<std::uint8_t>(~kRepeating);
        state |= kJustReleased;
    } else if (action == GLFW_REPEAT) {
        state |= kRepeating;
    }
}

template <typename States>
void publishPendingStates(States& current, States& pending) {
    current = pending;
    for (std::uint8_t& state : pending) {
        state &= (kPressed | kRepeating);
    }
}

bool hasState(const auto& states, int index, std::uint8_t state) {
    return index >= 0 &&
           static_cast<std::size_t>(index) < states.size() &&
           (states[static_cast<std::size_t>(index)] & state) != 0;
}

} // namespace

void InputState::setBindings(std::shared_ptr<InputBindings> bindings) {
    m_bindings = std::move(bindings);
}

void InputState::addListener(InputListener* listener) {
    if (!listener) {
        return;
    }
    if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end()) {
        m_listeners.push_back(listener);
    }
}

void InputState::removeListener(InputListener* listener) {
    auto it = std::remove(m_listeners.begin(), m_listeners.end(), listener);
    m_listeners.erase(it, m_listeners.end());
}

void InputState::handleKeyEvent(int key, int action) {
    if (key < 0 || key > GLFW_KEY_LAST) {
        return;
    }
    applyButtonEvent(m_pendingKeys[static_cast<std::size_t>(key)], action);
}

void InputState::handleMouseButtonEvent(int button, int action) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST) {
        return;
    }
    applyButtonEvent(m_pendingMouseButtons[static_cast<std::size_t>(button)], action);
}

void InputState::beginFrame() {
    publishPendingStates(m_currentKeys, m_pendingKeys);
    publishPendingStates(m_currentMouseButtons, m_pendingMouseButtons);
    dispatchActionTransitions();
}

bool InputState::isKeyPressed(int key) const {
    return hasState(m_currentKeys, key, kPressed);
}

bool InputState::isKeyJustPressed(int key) const {
    return hasState(m_currentKeys, key, kJustPressed);
}

bool InputState::isKeyJustReleased(int key) const {
    return hasState(m_currentKeys, key, kJustReleased);
}

bool InputState::isKeyRepeating(int key) const {
    return hasState(m_currentKeys, key, kRepeating);
}

bool InputState::isMouseButtonJustPressed(int button) const {
    return hasState(m_currentMouseButtons, button, kJustPressed);
}

bool InputState::isActionPressed(std::string_view action) const {
    auto key = resolveKey(action);
    return key && isKeyPressed(*key);
}

bool InputState::isActionJustPressed(std::string_view action) const {
    auto key = resolveKey(action);
    return key && isKeyJustPressed(*key);
}

bool InputState::isActionJustReleased(std::string_view action) const {
    auto key = resolveKey(action);
    return key && isKeyJustReleased(*key);
}

std::optional<int> InputState::resolveKey(std::string_view action) const {
    if (!m_bindings) {
        return std::nullopt;
    }
    return m_bindings->keyFor(action);
}

void InputState::dispatchActionTransitions() {
    if (!m_bindings) {
        return;
    }

    for (const auto& [action, key] : m_bindings->bindings()) {
        if (!key) {
            continue;
        }

        if (isKeyJustPressed(*key)) {
            for (InputListener* listener : m_listeners) {
                listener->onActionPressed(action);
            }
        }

        if (isKeyJustReleased(*key)) {
            for (InputListener* listener : m_listeners) {
                listener->onActionReleased(action);
            }
        }
    }
}

} // namespace Rigel::Input
