#pragma once

#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/input/PhysicalInput.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Input {

class InputBindings : public Asset::AssetBase {
public:
    void bind(const std::string& action, int key);
    void unbind(const std::string& action);
    void setBindings(
        const std::string& action,
        std::vector<PhysicalInput> inputs);

    bool hasAction(std::string_view action) const;
    bool isBound(std::string_view action) const;

    const std::unordered_map<std::string, std::vector<PhysicalInput>>&
    bindings() const {
        return m_bindings;
    }

private:
    std::unordered_map<std::string, std::vector<PhysicalInput>> m_bindings;
};

} // namespace Rigel::Input
