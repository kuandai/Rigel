#include "Rigel/input/InputDispatcher.h"
#include "Rigel/input/keypress.h"

#include <algorithm>

namespace Rigel::Input {

std::optional<int> InputDispatcher::resolveKey(std::string_view action) const {
    if (!m_bindings) {
        return std::nullopt;
    }
    return m_bindings->keyFor(action);
}

void InputDispatcher::setBindings(std::shared_ptr<InputBindings> bindings) {
    m_bindings = std::move(bindings);
}

void InputDispatcher::addListener(InputListener* listener) {
    if (!listener) {
        return;
    }
    if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end()) {
        m_listeners.push_back(listener);
    }
}

void InputDispatcher::removeListener(InputListener* listener) {
    auto it = std::remove(m_listeners.begin(), m_listeners.end(), listener);
    m_listeners.erase(it, m_listeners.end());
}

void InputDispatcher::update() {
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

bool InputDispatcher::isActionPressed(std::string_view action) const {
    auto key = resolveKey(action);
    return key && isKeyPressed(*key);
}

bool InputDispatcher::isActionJustPressed(std::string_view action) const {
    auto key = resolveKey(action);
    return key && isKeyJustPressed(*key);
}

bool InputDispatcher::isActionJustReleased(std::string_view action) const {
    auto key = resolveKey(action);
    return key && isKeyJustReleased(*key);
}

} // namespace Rigel::Input
