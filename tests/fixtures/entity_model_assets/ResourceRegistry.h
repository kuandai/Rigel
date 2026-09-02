#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ResourceRegistry {
public:
    static void SetEntityModel(std::string definition) {
        entityModel() = std::move(definition);
    }

    static std::span<const char> Get(const std::string& path) {
        static constexpr char manifest[] =
            "namespace: test\n"
            "assets:\n"
            "  entity_models:\n"
            "    authored:\n"
            "      path: models/entities/authored.yaml\n";

        if (path == "manifest.yaml") {
            return {manifest, sizeof(manifest) - 1};
        }
        if (path == "models/entities/authored.yaml") {
            const std::string& model = entityModel();
            return {model.data(), model.size()};
        }
        throw std::runtime_error("Resource not found: " + path);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> paths;
        return paths;
    }

private:
    static std::string& entityModel() {
        static std::string definition;
        return definition;
    }
};
