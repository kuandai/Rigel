#include "Rigel/input/InputBindings.h"

namespace Rigel {

void InputBindings::bind(const std::string& action, int key) {
    m_bindings[action] = key;
}

void InputBindings::unbind(const std::string& action) {
    m_bindings[action] = std::nullopt;
}

void InputBindings::setBinding(const std::string& action, std::optional<int> key) {
    m_bindings[action] = key;
}

bool InputBindings::hasAction(std::string_view action) const {
    return m_bindings.find(std::string(action)) != m_bindings.end();
}

bool InputBindings::isBound(std::string_view action) const {
    auto it = m_bindings.find(std::string(action));
    return it != m_bindings.end() && it->second.has_value();
}

std::optional<int> InputBindings::keyFor(std::string_view action) const {
    auto it = m_bindings.find(std::string(action));
    if (it == m_bindings.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace Rigel
