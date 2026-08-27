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

void InputState::setBindings(std::shared_ptr<const InputBindings> bindings) {
    if (bindings == m_bindings) {
        m_pendingBindings.reset();
        return;
    }
    m_pendingBindings = PendingBindings{
        std::move(bindings), m_pendingKeys, m_pendingMouseButtons};
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

void InputState::handleFocusLost() {
    auto releaseHeld = [](auto& states) {
        for (std::uint8_t& state : states) {
            if (state & kPressed) {
                state &= static_cast<std::uint8_t>(~(kPressed | kRepeating));
                state |= kJustReleased;
            }
        }
    };
    releaseHeld(m_pendingKeys);
    releaseHeld(m_pendingMouseButtons);
}

void InputState::beginFrame() {
    publishPendingStates(m_currentKeys, m_pendingKeys);
    publishPendingStates(m_currentMouseButtons, m_pendingMouseButtons);
    if (m_pendingBindings) {
        PendingBindings pending = std::move(*m_pendingBindings);
        m_pendingBindings.reset();
        m_bindings = std::move(pending.bindings);
        rebaseActionStates(
            pending.keysAtQueue, pending.mouseButtonsAtQueue);
    }
    updateActionTransitions();
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
    const auto state = m_actionStates.find(std::string(action));
    return state != m_actionStates.end() && (state->second & kPressed);
}

bool InputState::isActionJustPressed(std::string_view action) const {
    const auto state = m_actionStates.find(std::string(action));
    return state != m_actionStates.end() && (state->second & kJustPressed);
}

bool InputState::isActionJustReleased(std::string_view action) const {
    const auto state = m_actionStates.find(std::string(action));
    return state != m_actionStates.end() && (state->second & kJustReleased);
}

bool InputState::isPhysicalInputPressed(
    const PhysicalInput& input,
    const KeyStates& keys,
    const MouseButtonStates& mouseButtons) const {
    if (input.type == PhysicalInputType::Keyboard) {
        return hasState(keys, input.code, kPressed);
    }
    return hasState(mouseButtons, input.code, kPressed);
}

void InputState::rebaseActionStates(
    const KeyStates& keys,
    const MouseButtonStates& mouseButtons) {
    m_actionStates.clear();
    if (!m_bindings) {
        return;
    }
    for (const auto& [action, inputs] : m_bindings->bindings()) {
        const bool held = std::any_of(
            inputs.begin(), inputs.end(), [&](const PhysicalInput& input) {
                return isPhysicalInputPressed(input, keys, mouseButtons);
            });
        m_actionStates[action] = held ? kPressed : 0;
    }
}

void InputState::updateActionTransitions() {
    if (!m_bindings) {
        m_actionStates.clear();
        return;
    }
    std::unordered_map<std::string, std::uint8_t> nextStates;
    nextStates.reserve(m_bindings->bindings().size());
    for (const auto& [action, inputs] : m_bindings->bindings()) {
        const auto previous = m_actionStates.find(action);
        const bool wasHeld = previous != m_actionStates.end() &&
            (previous->second & kPressed);
        const bool held = std::any_of(
            inputs.begin(), inputs.end(), [&](const PhysicalInput& input) {
                return isPhysicalInputPressed(
                    input, m_currentKeys, m_currentMouseButtons);
            });
        std::uint8_t state = held ? kPressed : 0;
        if (held && !wasHeld) {
            state |= kJustPressed;
        } else if (!held && wasHeld) {
            state |= kJustReleased;
        } else if (!held && !wasHeld) {
            const bool tapped = std::any_of(
                inputs.begin(), inputs.end(), [&](const PhysicalInput& input) {
                    if (input.type == PhysicalInputType::Keyboard) {
                        return hasState(
                                   m_currentKeys,
                                   input.code,
                                   kJustPressed) &&
                            hasState(
                                   m_currentKeys,
                                   input.code,
                                   kJustReleased);
                    }
                    return hasState(
                               m_currentMouseButtons,
                               input.code,
                               kJustPressed) &&
                        hasState(
                               m_currentMouseButtons,
                               input.code,
                               kJustReleased);
                });
            if (tapped) {
                state |= kJustPressed | kJustReleased;
            }
        }
        nextStates.emplace(action, state);
    }
    m_actionStates = std::move(nextStates);
}

void InputState::dispatchActionTransitions() {
    for (const auto& [action, state] : m_actionStates) {
        if (state & kJustPressed) {
            for (InputListener* listener : m_listeners) {
                listener->onActionPressed(action);
            }
        }

        if (state & kJustReleased) {
            for (InputListener* listener : m_listeners) {
                listener->onActionReleased(action);
            }
        }
    }
}

} // namespace Rigel::Input
