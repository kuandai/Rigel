#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Asset/AssetIR.h"

#include <spdlog/spdlog.h>

#include <array>
#include <unordered_map>

namespace Rigel::Voxel {

namespace {

FaceTextures parseTexturesFromIR(const std::unordered_map<std::string, std::string>& textures,
                                 TextureAtlas& atlas) {
    FaceTextures out;

    auto get = [&](const char* key) -> const std::string* {
        auto it = textures.find(key);
        if (it == textures.end()) {
            return nullptr;
        }
        if (it->second.empty()) {
            return nullptr;
        }
        return &it->second;
    };

    if (const std::string* all = get("all")) {
        atlas.addTextureFromResource(*all);
        return FaceTextures::uniform(*all);
    }

    const std::string* top = get("top");
    const std::string* bottom = get("bottom");
    const std::string* sides = get("sides");
    if (top && bottom && sides) {
        atlas.addTextureFromResource(*top);
        atlas.addTextureFromResource(*bottom);
        atlas.addTextureFromResource(*sides);
        return FaceTextures::topBottomSides(*top, *bottom, *sides);
    }

    if (const std::string* def = get("default")) {
        atlas.addTextureFromResource(*def);
        out = FaceTextures::uniform(*def);
    }

    const std::array<std::pair<const char*, Direction>, 6> perFace = {{
        {"pos_x", Direction::PosX},
        {"neg_x", Direction::NegX},
        {"pos_y", Direction::PosY},
        {"neg_y", Direction::NegY},
        {"pos_z", Direction::PosZ},
        {"neg_z", Direction::NegZ}
    }};
    for (const auto& [key, direction] : perFace) {
        if (const std::string* tex = get(key)) {
            atlas.addTextureFromResource(*tex);
            out.faces[static_cast<size_t>(direction)] = *tex;
        }
    }

    return out;
}

} // namespace

size_t BlockLoader::loadFromManifest(
    Asset::AssetManager& assets,
    BlockRegistry& registry,
    TextureAtlas& atlas
) {
    (void)assets;
    size_t count = 0;

    spdlog::info("Loading block definitions from blocks directory...");

    Asset::IR::AssetGraphIR graph = Asset::IR::compileRigelEmbedded();
    const auto issues = Asset::IR::validate(graph);
    for (const auto& issue : issues) {
        if (issue.severity == Asset::IR::ValidationSeverity::Error) {
            spdlog::error("Block IR validation error [{}] {} (id='{}', field='{}')",
                          issue.sourcePath,
                          issue.message,
                          issue.identifier,
                          issue.field);
        } else {
            spdlog::warn("Block IR validation warning [{}] {} (id='{}', field='{}')",
                         issue.sourcePath,
                         issue.message,
                         issue.identifier,
                         issue.field);
        }
    }

    for (const auto& block : graph.blocks) {
        for (const auto& state : block.states) {
            try {
                if (state.identifier.empty()) {
                    spdlog::error("Skipping block state with empty identifier from '{}'", state.sourcePath);
                    continue;
                }
                if (registry.hasIdentifier(state.identifier)) {
                    spdlog::warn("Skipping duplicate block identifier '{}'", state.identifier);
                    continue;
                }

                BlockType type;
                type.identifier = state.identifier;
                type.model = state.model;
                type.layer = parseRenderLayer(state.renderLayer);
                type.isOpaque = state.isOpaque;
                type.isSolid = state.isSolid;
                type.cullSameType = state.cullSameType;
                type.emittedLight = state.emittedLight;
                type.lightAttenuation = state.lightAttenuation;
                type.textures = parseTexturesFromIR(state.textures, atlas);

                BlockID id = registry.registerBlock(type.identifier, std::move(type));
                spdlog::debug("Registered block '{}' from '{}' with ID {}",
                              state.identifier,
                              state.sourcePath,
                              id.type);
                ++count;
            } catch (const std::exception& e) {
                spdlog::error("Failed to register block '{}' from '{}': {}",
                              state.identifier,
                              state.sourcePath,
                              e.what());
            }
        }
    }

    spdlog::info("Loaded {} block definitions", count);
    return count;
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
