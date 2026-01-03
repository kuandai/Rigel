#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Util/Ryml.h"

#include <spdlog/spdlog.h>
#include <ryml.hpp>
#include <ryml_std.hpp>

namespace Rigel::Voxel {

namespace {
    // Helper to convert csubstr to std::string
    std::string toStdString(ryml::csubstr s) {
        return std::string(s.data(), s.size());
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

    spdlog::info("Loading block definitions from manifest...");

    assets.forEachInCategory("blocks", [&](const std::string& name, const Asset::AssetManager::AssetEntry& entry) {
        try {
            BlockType blockType = parseBlockType(name, entry, ns, atlas);

            BlockID id = registry.registerBlock(blockType.identifier, std::move(blockType));
            spdlog::debug("Registered block '{}' with ID {}", name, id.type);

            ++count;
        } catch (const std::exception& e) {
            spdlog::error("Failed to load block '{}': {}", name, e.what());
        }
    });

    spdlog::info("Loaded {} block definitions", count);
    return count;
}

BlockType BlockLoader::parseBlockType(
    const std::string& name,
    const Asset::AssetManager::AssetEntry& entry,
    const std::string& ns,
    TextureAtlas& atlas
) {
    BlockType type;

    // Build fully qualified identifier (namespace:name)
    type.identifier = ns.empty() ? name : ns + ":" + name;

    // Parse model (default: "cube")
    if (auto model = entry.getString("model")) {
        type.model = *model;
    }

    // Parse boolean properties
    if (auto opaque = getOptionalBool(entry.config, "opaque")) {
        type.isOpaque = *opaque;
    }

    if (auto solid = getOptionalBool(entry.config, "solid")) {
        type.isSolid = *solid;
    }

    if (auto cullSameType = getOptionalBool(entry.config, "cull_same_type")) {
        type.cullSameType = *cullSameType;
    }

    // Parse render layer
    if (auto layer = entry.getString("layer")) {
        type.layer = parseRenderLayer(*layer);
    }

    // Parse lighting properties
    if (auto emit = getOptionalString(entry.config, "emits_light")) {
        try {
            type.emittedLight = static_cast<uint8_t>(std::stoi(*emit));
        } catch (...) {
            type.emittedLight = 0;
        }
    }

    if (auto atten = getOptionalString(entry.config, "light_attenuation")) {
        try {
            type.lightAttenuation = static_cast<uint8_t>(std::stoi(*atten));
        } catch (...) {
            type.lightAttenuation = 15;
        }
    }

    // Parse textures
    if (entry.hasChild("textures")) {
        type.textures = parseTextures(entry.config["textures"], atlas);
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
