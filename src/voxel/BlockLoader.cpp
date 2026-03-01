#include "Rigel/Voxel/BlockLoader.h"
#include "Rigel/Asset/AssetIR.h"

#include <spdlog/spdlog.h>

#include <array>
#include <string_view>
#include <unordered_map>

namespace Rigel::Voxel {

namespace {

void tryAddTexture(TextureAtlas& atlas,
                   const std::string& texturePath,
                   const std::string& stateIdentifier) {
    if (texturePath.empty()) {
        return;
    }
    try {
        atlas.addTextureFromResource(texturePath);
    } catch (const std::exception& e) {
        spdlog::warn("BlockLoader: failed to register texture '{}' for '{}': {}",
                     texturePath,
                     stateIdentifier,
                     e.what());
    }
}

FaceTextures parseTexturesFromIR(const std::unordered_map<std::string, std::string>& textures,
                                 TextureAtlas& atlas,
                                 const std::string& stateIdentifier) {
    FaceTextures out;

    auto get = [&](std::string_view key) -> const std::string* {
        auto it = textures.find(std::string(key));
        if (it == textures.end()) {
            return nullptr;
        }
        if (it->second.empty()) {
            return nullptr;
        }
        return &it->second;
    };

    auto getFirst = [&](std::initializer_list<std::string_view> keys) -> const std::string* {
        for (std::string_view key : keys) {
            if (const std::string* value = get(key)) {
                return value;
            }
        }
        return nullptr;
    };

    if (const std::string* all = getFirst({"all"})) {
        tryAddTexture(atlas, *all, stateIdentifier);
        return FaceTextures::uniform(*all);
    }

    const std::string* top = get("top");
    const std::string* bottom = get("bottom");
    const std::string* sides = getFirst({"sides", "side"});
    if (top && bottom && sides) {
        tryAddTexture(atlas, *top, stateIdentifier);
        tryAddTexture(atlas, *bottom, stateIdentifier);
        tryAddTexture(atlas, *sides, stateIdentifier);
        return FaceTextures::topBottomSides(*top, *bottom, *sides);
    }

    if (const std::string* def = getFirst({"default", "albedo", "diffuse"})) {
        tryAddTexture(atlas, *def, stateIdentifier);
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
            tryAddTexture(atlas, *tex, stateIdentifier);
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
                type.textures = parseTexturesFromIR(state.textures, atlas, state.identifier);

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
