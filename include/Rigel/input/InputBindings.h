#pragma once

#include "Rigel/Asset/AssetLoader.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Rigel::Input {

class InputBindings : public Asset::AssetBase {
public:
    void bind(const std::string& action, int key);
    void unbind(const std::string& action);
    void setBinding(const std::string& action, std::optional<int> key);

    bool hasAction(std::string_view action) const;
    bool isBound(std::string_view action) const;
    std::optional<int> keyFor(std::string_view action) const;

    const std::unordered_map<std::string, std::optional<int>>& bindings() const {
        return m_bindings;
    }

private:
    std::unordered_map<std::string, std::optional<int>> m_bindings;
};

} // namespace Rigel::Input
