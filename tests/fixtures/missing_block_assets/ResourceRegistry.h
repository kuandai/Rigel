#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class ResourceRegistry {
public:
    enum class Scenario {
        MissingTexture,
        TexturelessBlock,
    };

    static void SetScenario(Scenario value) {
        scenario() = value;
    }

    static std::span<const char> Get(const std::string& path) {
        static constexpr char missingTextureDefinition[] =
            "id: required_block\n"
            "model: cube\n"
            "opaque: true\n"
            "solid: true\n"
            "textures:\n"
            "  all: textures/blocks/required.png\n";
        static constexpr char texturelessModel[] =
            "id: invisible_shape\n"
            "texture_slots: []\n"
            "cuboids:\n"
            "  - bounds: [0, 0, 0, 1, 1, 1]\n"
            "    faces: {}\n";
        static constexpr char texturelessBlock[] =
            "id: invisible_block\n"
            "model: invisible_shape\n"
            "opaque: false\n"
            "solid: false\n"
            "textures: {}\n";
        static constexpr unsigned char inventedTexture[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72,
            68, 82, 0, 0, 0, 16, 0, 0, 0, 16, 8, 6, 0, 0, 0, 31,
            243, 255, 97, 0, 0, 0, 25, 73, 68, 65, 84, 120, 218, 99,
            144, 175, 191, 255, 159, 18, 204, 48, 106, 192, 168, 1,
            163, 6, 12, 23, 3, 0, 175, 166, 124, 31, 213, 197, 7,
            251, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
        };

        if (path == "blocks/required_block.yaml") {
            return std::span<const char>(
                missingTextureDefinition, sizeof(missingTextureDefinition) - 1);
        }
        if (path == "models/blocks/invisible_shape.yaml") {
            return std::span<const char>(
                texturelessModel, sizeof(texturelessModel) - 1);
        }
        if (path == "blocks/invisible_block.yaml") {
            return std::span<const char>(
                texturelessBlock, sizeof(texturelessBlock) - 1);
        }
        if (path == "textures/invented/a_available.png") {
            return std::span<const char>(
                reinterpret_cast<const char*>(inventedTexture),
                sizeof(inventedTexture));
        }
        throw std::runtime_error("Resource not found: " + path);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> missingTexturePaths = {
            "blocks/required_block.yaml"
        };
        static const std::vector<std::string_view> texturelessPaths = {
            "models/blocks/invisible_shape.yaml",
            "blocks/invisible_block.yaml",
        };
        return scenario() == Scenario::MissingTexture
            ? missingTexturePaths
            : texturelessPaths;
    }

private:
    static Scenario& scenario() {
        static Scenario value = Scenario::MissingTexture;
        return value;
    }
};
