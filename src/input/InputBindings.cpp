#include "Rigel/input/InputBindings.h"

#include <utility>

namespace Rigel::Input {

void InputBindings::bind(const std::string& action, int key) {
    m_bindings[action] = {{PhysicalInputType::Keyboard, key}};
}

void InputBindings::unbind(const std::string& action) {
    m_bindings[action] = {};
}

void InputBindings::setBindings(
    const std::string& action,
    std::vector<PhysicalInput> inputs) {
    m_bindings[action] = std::move(inputs);
}

bool InputBindings::hasAction(std::string_view action) const {
    return m_bindings.find(std::string(action)) != m_bindings.end();
}

bool InputBindings::isBound(std::string_view action) const {
    auto it = m_bindings.find(std::string(action));
    return it != m_bindings.end() && !it->second.empty();
}

} // namespace Rigel::Input
