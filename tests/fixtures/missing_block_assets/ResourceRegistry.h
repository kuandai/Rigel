#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class ResourceRegistry {
public:
    static std::span<const char> Get(const std::string& path) {
        static constexpr char definition[] =
            "id: required_block\n"
            "model: cube\n"
            "opaque: true\n"
            "solid: true\n"
            "textures:\n"
            "  all: textures/blocks/required.png\n";

        if (path == "blocks/required_block.yaml") {
            return std::span<const char>(definition, sizeof(definition) - 1);
        }
        throw std::runtime_error("Resource not found: " + path);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> paths = {
            "blocks/required_block.yaml"
        };
        return paths;
    }
};
