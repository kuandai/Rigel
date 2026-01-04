#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Util/Ryml.h"
#include "ResourceRegistry.h"

#include <spdlog/spdlog.h>
#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

namespace Rigel::Voxel {

namespace {
    ryml::Tree loadBlockConfigTree(const std::string& path) {
        auto data = ResourceRegistry::Get(path);
        return ryml::parse_in_arena(
            ryml::to_csubstr(path.c_str()),
            ryml::csubstr(data.data(), data.size())
        );
    }

    bool startsWith(std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() &&
               value.compare(0, prefix.size(), prefix) == 0;
    }

    bool endsWith(std::string_view value, std::string_view suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string blockNameFromPath(const std::string& path) {
        static constexpr std::string_view kPrefix = "blocks/";
        static constexpr std::string_view kSuffix = ".yaml";
        std::string_view view(path);
        if (startsWith(view, kPrefix)) {
            view.remove_prefix(kPrefix.size());
        }
        if (endsWith(view, kSuffix)) {
            view.remove_suffix(kSuffix.size());
        }
        return std::string(view);
    }

    // Helper to read optional string from config
    std::optional<std::string> getOptionalString(ryml::ConstNodeRef node, const char* key) {
        if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
            return std::nullopt;
        }
        std::string value;
        node[ryml::to_csubstr(key)] >> value;
        return value;
    }

    // Helper to read optional bool from config
    std::optional<bool> getOptionalBool(ryml::ConstNodeRef node, const char* key) {
        if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
            return std::nullopt;
        }
        std::string value;
        node[ryml::to_csubstr(key)] >> value;
        return value == "true" || value == "yes" || value == "1";
    }
}

size_t BlockLoader::loadFromManifest(
    Asset::AssetManager& assets,
    BlockRegistry& registry,
    TextureAtlas& atlas
) {
    size_t count = 0;
    const std::string& ns = assets.ns();

    spdlog::info("Loading block definitions from blocks directory...");

    std::vector<std::string> blockPaths;
    for (std::string_view path : ResourceRegistry::Paths()) {
        if (!startsWith(path, "blocks/") || !endsWith(path, ".yaml")) {
            continue;
        }
        blockPaths.emplace_back(path);
    }
    std::sort(blockPaths.begin(), blockPaths.end());

    for (const auto& path : blockPaths) {
        try {
            ryml::Tree blockTree = loadBlockConfigTree(path);
            ryml::ConstNodeRef config = blockTree.rootref();
            std::string name = blockNameFromPath(path);
            BlockType blockType = parseBlockType(name, config, ns, atlas);

            std::string identifier = blockType.identifier;
            if (registry.hasIdentifier(identifier)) {
                spdlog::warn("Skipping duplicate block identifier '{}'", identifier);
                continue;
            }
            BlockID id = registry.registerBlock(identifier, std::move(blockType));
            spdlog::debug("Registered block '{}' from '{}' with ID {}", name, path, id.type);

            ++count;
        } catch (const std::exception& e) {
            spdlog::error("Failed to load block from '{}': {}", path, e.what());
        }
    }

    spdlog::info("Loaded {} block definitions", count);
    return count;
}

BlockType BlockLoader::parseBlockType(
    const std::string& name,
    ryml::ConstNodeRef config,
    const std::string& ns,
    TextureAtlas& atlas
) {
    BlockType type;

    // Build fully qualified identifier (namespace:name)
    if (auto explicitId = getOptionalString(config, "id")) {
        type.identifier = *explicitId;
    } else if (auto explicitId = getOptionalString(config, "identifier")) {
        type.identifier = *explicitId;
    } else if (auto explicitName = getOptionalString(config, "name")) {
        type.identifier = *explicitName;
    } else {
        type.identifier = ns.empty() ? name : ns + ":" + name;
    }

    if (type.identifier.find(':') == std::string::npos && !ns.empty()) {
        type.identifier = ns + ":" + type.identifier;
    }

    // Parse model (default: "cube")
    if (auto model = getOptionalString(config, "model")) {
        type.model = *model;
    }

    // Parse boolean properties
    if (auto opaque = getOptionalBool(config, "opaque")) {
        type.isOpaque = *opaque;
    }

    if (auto solid = getOptionalBool(config, "solid")) {
        type.isSolid = *solid;
    }

    if (auto cullSameType = getOptionalBool(config, "cull_same_type")) {
        type.cullSameType = *cullSameType;
    }

    // Parse render layer
    if (auto layer = getOptionalString(config, "layer")) {
        type.layer = parseRenderLayer(*layer);
    }

    // Parse lighting properties
    if (auto emit = getOptionalString(config, "emits_light")) {
        try {
            type.emittedLight = static_cast<uint8_t>(std::stoi(*emit));
        } catch (...) {
            type.emittedLight = 0;
        }
    }

    if (auto atten = getOptionalString(config, "light_attenuation")) {
        try {
            type.lightAttenuation = static_cast<uint8_t>(std::stoi(*atten));
        } catch (...) {
            type.lightAttenuation = 15;
        }
    }

    // Parse textures
    if (config.has_child("textures")) {
        type.textures = parseTextures(config["textures"], atlas);
    }

    return type;
}

FaceTextures BlockLoader::parseTextures(
    ryml::ConstNodeRef config,
    TextureAtlas& atlas
) {
    FaceTextures textures;

    // Check for "all" first (uniform texture)
    if (config.has_child("all")) {
        std::string path;
        config["all"] >> path;

        // Add to atlas and store path
        atlas.addTextureFromResource(path);
        textures = FaceTextures::uniform(path);

        return textures;
    }

    // Check for top/bottom/sides pattern
    std::optional<std::string> topPath, bottomPath, sidesPath;

    if (config.has_child("top")) {
        std::string path;
        config["top"] >> path;
        topPath = path;
        atlas.addTextureFromResource(path);
    }

    if (config.has_child("bottom")) {
        std::string path;
        config["bottom"] >> path;
        bottomPath = path;
        atlas.addTextureFromResource(path);
    }

    if (config.has_child("sides")) {
        std::string path;
        config["sides"] >> path;
        sidesPath = path;
        atlas.addTextureFromResource(path);
    }

    // If we have top/bottom/sides pattern
    if (topPath && bottomPath && sidesPath) {
        textures = FaceTextures::topBottomSides(*topPath, *bottomPath, *sidesPath);
        return textures;
    }

    // Otherwise, try individual face textures
    const std::array<std::pair<const char*, Direction>, 6> faceKeys = {{
        {"pos_x", Direction::PosX},
        {"neg_x", Direction::NegX},
        {"pos_y", Direction::PosY},
        {"neg_y", Direction::NegY},
        {"pos_z", Direction::PosZ},
        {"neg_z", Direction::NegZ}
    }};

    // Start with default empty paths
    std::string defaultPath;
    if (config.has_child("default")) {
        config["default"] >> defaultPath;
        atlas.addTextureFromResource(defaultPath);
        textures = FaceTextures::uniform(defaultPath);
    }

    // Override individual faces
    for (const auto& [key, dir] : faceKeys) {
        if (config.has_child(ryml::to_csubstr(key))) {
            std::string path;
            config[ryml::to_csubstr(key)] >> path;
            atlas.addTextureFromResource(path);
            textures.faces[static_cast<size_t>(dir)] = path;
        }
    }

    return textures;
}

RenderLayer BlockLoader::parseRenderLayer(const std::string& str) {
    if (str == "opaque") return RenderLayer::Opaque;
    if (str == "cutout") return RenderLayer::Cutout;
    if (str == "transparent") return RenderLayer::Transparent;
    if (str == "emissive") return RenderLayer::Emissive;

    spdlog::warn("Unknown render layer '{}', defaulting to opaque", str);
    return RenderLayer::Opaque;
}

} // namespace Rigel::Voxel
