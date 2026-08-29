#include "Rigel/Voxel/BlockLoader.h"

#include "ResourceRegistry.h"
#include "Rigel/Util/Ryml.h"

#include <ryml.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rigel::Voxel {
namespace {

constexpr size_t kRepresentativeFailureLimit = 3;

class BlockYamlParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throwBlockYamlParseError(
    const char* message, size_t length, ryml::Location, void*
) {
    throw BlockYamlParseError(std::string(message, length));
}

ryml::Tree parseYaml(std::string_view path, std::span<const char> data) {
    ryml::Callbacks callbacks = ryml::get_callbacks();
    callbacks.m_error = &throwBlockYamlParseError;
    ryml::Tree tree(callbacks);
    ryml::Parser::handler_type handler(callbacks);
    ryml::Parser parser(&handler);
    try {
        ryml::parse_in_arena(
            &parser,
            ryml::csubstr(path.data(), path.size()),
            ryml::csubstr(data.data(), data.size()),
            &tree);
    } catch (const BlockYamlParseError& error) {
        throw std::invalid_argument(
            "Invalid YAML in '" + std::string(path) + "': " + error.what());
    }
    return tree;
}

[[noreturn]] void fail(
    std::string_view source, std::string_view field, std::string_view reason
) {
    throw std::invalid_argument(
        "Invalid normalized block field '" + std::string(field) + "' in '" +
        std::string(source) + "': " + std::string(reason));
}

bool contains(
    std::initializer_list<std::string_view> values, std::string_view value
) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void requireMap(
    ryml::ConstNodeRef node,
    std::string_view source,
    std::string_view path,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required = {}
) {
    if (!node.readable() || !node.is_map()) {
        fail(source, path, "expected a mapping");
    }
    std::unordered_set<std::string> encountered;
    for (const ryml::ConstNodeRef child : node.children()) {
        const std::string key = Util::toStdString(child.key());
        if (!encountered.insert(key).second) {
            fail(source, std::string(path) + "." + key, "duplicate field");
        }
        if (!contains(allowed, key)) {
            fail(source, std::string(path) + "." + key, "unknown field");
        }
    }
    for (const std::string_view field : required) {
        if (!node.has_child(ryml::csubstr(field.data(), field.size()))) {
            fail(source, std::string(path) + "." + std::string(field),
                 "missing required field");
        }
    }
}

void requireSequence(
    ryml::ConstNodeRef node, std::string_view source, std::string_view path
) {
    if (!node.readable() || !node.is_seq()) {
        fail(source, path, "expected a sequence");
    }
}

ryml::ConstNodeRef child(ryml::ConstNodeRef node, std::string_view key) {
    return node[ryml::csubstr(key.data(), key.size())];
}

std::string scalar(
    ryml::ConstNodeRef node, std::string_view source, std::string_view path
) {
    if (!node.readable() || !node.has_val() || node.has_children()) {
        fail(source, path, "expected a scalar");
    }
    return Util::toStdString(node.val());
}

std::optional<std::string> optionalScalar(
    ryml::ConstNodeRef node,
    std::string_view key,
    std::string_view source,
    std::string_view path
) {
    if (!node.has_child(ryml::csubstr(key.data(), key.size()))) {
        return std::nullopt;
    }
    return scalar(child(node, key), source,
                  std::string(path) + "." + std::string(key));
}

bool booleanValue(
    ryml::ConstNodeRef node, std::string_view source, std::string_view path
) {
    const std::string value = scalar(node, source, path);
    if (value == "true") return true;
    if (value == "false") return false;
    fail(source, path, "expected boolean 'true' or 'false'");
}

std::optional<bool> optionalBool(
    ryml::ConstNodeRef node,
    std::string_view key,
    std::string_view source,
    std::string_view path
) {
    if (!node.has_child(ryml::csubstr(key.data(), key.size()))) {
        return std::nullopt;
    }
    return booleanValue(child(node, key), source,
                        std::string(path) + "." + std::string(key));
}

float floatValue(
    ryml::ConstNodeRef node, std::string_view source, std::string_view path
) {
    const std::string value = scalar(node, source, path);
    float parsed = 0.0f;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed,
        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        fail(source, path, "expected a finite number");
    }
    return parsed;
}

int integerValue(
    ryml::ConstNodeRef node, std::string_view source, std::string_view path
) {
    const std::string value = scalar(node, source, path);
    int parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        fail(source, path, "expected an integer");
    }
    return parsed;
}

std::string qualify(std::string identifier, std::string_view assetNamespace) {
    if (!identifier.empty() && identifier.find(':') == std::string::npos &&
        !assetNamespace.empty()) {
        identifier = std::string(assetNamespace) + ":" + identifier;
    }
    return identifier;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.starts_with(prefix);
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.ends_with(suffix);
}

std::string blockNameFromPath(std::string_view path) {
    constexpr std::string_view prefix = "blocks/";
    constexpr std::string_view suffix = ".yaml";
    if (startsWith(path, prefix)) path.remove_prefix(prefix.size());
    if (endsWith(path, suffix)) path.remove_suffix(suffix.size());
    return std::string(path);
}

void addFailure(
    BlockLoadReport& report, std::string_view path, std::string reason
) {
    if (report.representativeFailures.size() < kRepresentativeFailureLimit) {
        report.representativeFailures.push_back(
            {std::string(path), std::move(reason)});
    }
}

std::shared_ptr<const BlockModel> parseModel(
    std::string_view source,
    std::span<const char> data,
    std::string_view assetNamespace
) {
    ryml::Tree tree = parseYaml(source, data);
    const ryml::ConstNodeRef root = tree.rootref();
    requireMap(root, source, "model", {"id", "texture_slots", "cuboids"},
               {"id", "texture_slots", "cuboids"});

    std::string identifier = qualify(
        scalar(child(root, "id"), source, "model.id"), assetNamespace);
    if (identifier.empty()) {
        fail(source, "model.id", "expected a non-empty identifier");
    }

    const ryml::ConstNodeRef slotsNode = child(root, "texture_slots");
    requireSequence(slotsNode, source, "model.texture_slots");
    std::vector<std::string> textureSlots;
    std::unordered_set<std::string> knownSlots;
    size_t slotIndex = 0;
    for (const ryml::ConstNodeRef slotNode : slotsNode.children()) {
        const std::string path =
            "model.texture_slots[" + std::to_string(slotIndex) + "]";
        std::string slot = scalar(slotNode, source, path);
        if (slot.empty()) fail(source, path, "expected a non-empty slot name");
        if (!knownSlots.insert(slot).second) fail(source, path, "duplicate texture slot");
        textureSlots.push_back(std::move(slot));
        ++slotIndex;
    }

    const ryml::ConstNodeRef cuboidsNode = child(root, "cuboids");
    requireSequence(cuboidsNode, source, "model.cuboids");
    if (cuboidsNode.num_children() == 0) {
        fail(source, "model.cuboids", "expected at least one cuboid");
    }

    std::vector<BlockModelCuboid> cuboids;
    cuboids.reserve(cuboidsNode.num_children());
    size_t cuboidIndex = 0;
    for (const ryml::ConstNodeRef cuboidNode : cuboidsNode.children()) {
        const std::string cuboidPath =
            "model.cuboids[" + std::to_string(cuboidIndex) + "]";
        requireMap(cuboidNode, source, cuboidPath, {"bounds", "faces"},
                   {"bounds", "faces"});

        const ryml::ConstNodeRef boundsNode = child(cuboidNode, "bounds");
        requireSequence(boundsNode, source, cuboidPath + ".bounds");
        if (boundsNode.num_children() != 6) {
            fail(source, cuboidPath + ".bounds", "expected six coordinates");
        }
        std::array<float, 6> values{};
        for (size_t index = 0; index < values.size(); ++index) {
            values[index] = floatValue(
                boundsNode[index], source,
                cuboidPath + ".bounds[" + std::to_string(index) + "]");
        }
        BlockModelCuboid cuboid;
        cuboid.bounds.min = {values[0], values[1], values[2]};
        cuboid.bounds.max = {values[3], values[4], values[5]};
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!(cuboid.bounds.min[axis] < cuboid.bounds.max[axis])) {
                fail(source, cuboidPath + ".bounds",
                     "cuboid minimum must be less than maximum on every axis");
            }
        }

        const ryml::ConstNodeRef facesNode = child(cuboidNode, "faces");
        if (!facesNode.readable() || !facesNode.is_map()) {
            fail(source, cuboidPath + ".faces", "expected a mapping");
        }
        std::unordered_set<std::string> encounteredFaces;
        for (const ryml::ConstNodeRef faceNode : facesNode.children()) {
            const std::string faceName = Util::toStdString(faceNode.key());
            const std::string facePath = cuboidPath + ".faces." + faceName;
            if (!encounteredFaces.insert(faceName).second) {
                fail(source, facePath, "duplicate face");
            }
            const auto direction = BlockModel::directionFromName(faceName);
            if (!direction) fail(source, facePath, "invalid cardinal face");
            requireMap(
                faceNode, source, facePath,
                {"texture", "uv", "rotation", "ambient_occlusion", "cull"},
                {"texture"});

            BlockModelFace face;
            face.textureSlot = scalar(
                child(faceNode, "texture"), source, facePath + ".texture");
            if (!knownSlots.contains(face.textureSlot)) {
                fail(source, facePath + ".texture",
                     "unresolved texture slot '" + face.textureSlot + "'");
            }
            if (faceNode.has_child("uv")) {
                const ryml::ConstNodeRef uvNode = child(faceNode, "uv");
                requireSequence(uvNode, source, facePath + ".uv");
                if (uvNode.num_children() != 4) {
                    fail(source, facePath + ".uv", "expected four coordinates");
                }
                std::array<float, 4> uv{};
                for (size_t index = 0; index < uv.size(); ++index) {
                    uv[index] = floatValue(
                        uvNode[index], source,
                        facePath + ".uv[" + std::to_string(index) + "]");
                    if (uv[index] < 0.0f || uv[index] > 1.0f) {
                        fail(source, facePath + ".uv",
                             "coordinates must be within [0, 1]");
                    }
                }
                if (uv[0] == uv[2] || uv[1] == uv[3]) {
                    fail(source, facePath + ".uv", "rectangle must have non-zero area");
                }
                face.uv = {uv[0], uv[1], uv[2], uv[3]};
            }
            if (faceNode.has_child("rotation")) {
                const int degrees = integerValue(
                    child(faceNode, "rotation"), source, facePath + ".rotation");
                switch (degrees) {
                    case 0: face.rotation = BlockModelUvRotation::None; break;
                    case 90: face.rotation = BlockModelUvRotation::Quarter; break;
                    case 180: face.rotation = BlockModelUvRotation::Half; break;
                    case 270: face.rotation = BlockModelUvRotation::ThreeQuarter; break;
                    default:
                        fail(source, facePath + ".rotation",
                             "expected 0, 90, 180, or 270 degrees");
                }
            }
            if (faceNode.has_child("ambient_occlusion")) {
                face.ambientOcclusion = booleanValue(
                    child(faceNode, "ambient_occlusion"), source,
                    facePath + ".ambient_occlusion");
            }
            if (faceNode.has_child("cull")) {
                face.cullAgainstOpaqueNeighbor = booleanValue(
                    child(faceNode, "cull"), source, facePath + ".cull");
            }
            cuboid.faces[static_cast<size_t>(*direction)] = std::move(face);
        }
        cuboids.push_back(std::move(cuboid));
        ++cuboidIndex;
    }

    return std::make_shared<const BlockModel>(
        std::move(identifier), std::move(textureSlots), std::move(cuboids));
}

struct ParsedBlock {
    std::string source;
    std::string identifier;
    BlockType type;
    std::vector<std::string> texturePaths;
};

std::unordered_map<std::string, std::string> parseTextureMap(
    ryml::ConstNodeRef node, std::string_view source
) {
    if (!node.readable() || !node.is_map()) {
        fail(source, "block.textures", "expected a mapping");
    }
    std::unordered_map<std::string, std::string> result;
    for (const ryml::ConstNodeRef binding : node.children()) {
        std::string slot = Util::toStdString(binding.key());
        std::string path = scalar(
            binding, source, "block.textures." + slot);
        if (path.empty()) {
            fail(source, "block.textures." + slot,
                 "expected a non-empty resource path");
        }
        if (!result.emplace(slot, std::move(path)).second) {
            fail(source, "block.textures." + slot, "duplicate texture binding");
        }
    }
    return result;
}

FaceTextures bindCubeTextures(
    const std::unordered_map<std::string, std::string>& bindings,
    std::string_view source
) {
    static constexpr std::array<std::string_view, 11> allowed = {
        "all", "default", "top", "bottom", "sides",
        "pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z"
    };
    for (const auto& [key, unused] : bindings) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            fail(source, "block.textures." + key,
                 "unknown full-cube texture binding");
        }
    }

    FaceTextures result;
    if (const auto all = bindings.find("all"); all != bindings.end()) {
        if (bindings.size() != 1) {
            fail(source, "block.textures", "'all' cannot be combined with other bindings");
        }
        return FaceTextures::uniform(all->second);
    }

    const bool hasPattern = bindings.contains("top") ||
        bindings.contains("bottom") || bindings.contains("sides");
    if (hasPattern) {
        if (!bindings.contains("top") || !bindings.contains("bottom") ||
            !bindings.contains("sides") || bindings.size() != 3) {
            fail(source, "block.textures",
                 "top, bottom, and sides must be provided together");
        }
        return FaceTextures::topBottomSides(
            bindings.at("top"), bindings.at("bottom"), bindings.at("sides"));
    }

    if (const auto fallback = bindings.find("default"); fallback != bindings.end()) {
        result = FaceTextures::uniform(fallback->second);
    }
    for (size_t index = 0; index < DirectionCount; ++index) {
        const Direction direction = static_cast<Direction>(index);
        const auto binding = bindings.find(
            std::string(BlockModel::directionName(direction)));
        if (binding != bindings.end()) result.setFace(direction, binding->second);
        if (result.forFace(direction).empty()) {
            fail(source,
                 "block.textures." +
                     std::string(BlockModel::directionName(direction)),
                 "unresolved texture slot");
        }
    }
    return result;
}

ParsedBlock parseBlock(
    std::string_view source,
    std::span<const char> data,
    std::string_view assetNamespace,
    const std::unordered_map<std::string, std::shared_ptr<const BlockModel>>& stagedModels,
    const BlockModelRegistry& models
) {
    ryml::Tree tree = parseYaml(source, data);
    const ryml::ConstNodeRef root = tree.rootref();
    requireMap(
        root, source, "block",
        {"id", "identifier", "name", "model", "opaque", "solid",
         "cull_same_type", "layer", "emits_light", "light_attenuation",
         "textures"});

    size_t explicitNames = 0;
    std::string identifier;
    for (const std::string_view key : {"id", "identifier", "name"}) {
        if (const auto value = optionalScalar(root, key, source, "block")) {
            identifier = *value;
            ++explicitNames;
        }
    }
    if (explicitNames > 1) {
        fail(source, "block.id", "declare only one of id, identifier, or name");
    }
    if (identifier.empty()) identifier = blockNameFromPath(source);
    identifier = qualify(std::move(identifier), assetNamespace);

    std::string modelId = optionalScalar(root, "model", source, "block")
        .value_or("cube");
    std::shared_ptr<const BlockModel> model;
    if (modelId == "cube" || modelId == "none") {
        model = models.find(modelId);
    } else {
        modelId = qualify(std::move(modelId), assetNamespace);
        const auto staged = stagedModels.find(modelId);
        model = staged == stagedModels.end() ? models.find(modelId) : staged->second;
    }
    if (!model) {
        fail(source, "block.model", "unknown model '" + modelId + "'");
    }

    BlockType type;
    type.identifier = identifier;
    type.model = model;
    if (const auto value = optionalBool(root, "opaque", source, "block")) {
        type.isOpaque = *value;
    }
    if (const auto value = optionalBool(root, "solid", source, "block")) {
        type.isSolid = *value;
    }
    if (const auto value = optionalBool(root, "cull_same_type", source, "block")) {
        type.cullSameType = *value;
    }
    if (const auto layer = optionalScalar(root, "layer", source, "block")) {
        if (*layer == "opaque") type.layer = RenderLayer::Opaque;
        else if (*layer == "cutout") type.layer = RenderLayer::Cutout;
        else if (*layer == "transparent") type.layer = RenderLayer::Transparent;
        else if (*layer == "emissive") type.layer = RenderLayer::Emissive;
        else fail(source, "block.layer", "unknown render layer '" + *layer + "'");
    }
    for (const auto [key, destination] : {
             std::pair{"emits_light", &type.emittedLight},
             std::pair{"light_attenuation", &type.lightAttenuation}}) {
        if (root.has_child(key)) {
            const int value = integerValue(
                root[key], source, "block." + std::string(key));
            if (value < 0 || value > 15) {
                fail(source, "block." + std::string(key),
                     "expected an integer from 0 to 15");
            }
            *destination = static_cast<uint8_t>(value);
        }
    }

    std::unordered_map<std::string, std::string> bindings;
    if (root.has_child("textures")) {
        bindings = parseTextureMap(root["textures"], source);
    }
    if (model->isFullCube()) {
        type.textures = bindCubeTextures(bindings, source);
    } else {
        std::unordered_set<std::string> required(
            model->textureSlots().begin(), model->textureSlots().end());
        for (const auto& [slot, path] : bindings) {
            if (!required.erase(slot)) {
                fail(source, "block.textures." + slot,
                     "texture slot is not declared by model '" + modelId + "'");
            }
            type.textures.bind(slot, path);
        }
        for (const std::string& slot : model->textureSlots()) {
            if (required.contains(slot)) {
                fail(source, "block.textures." + slot,
                     "unresolved texture slot for model '" + modelId + "'");
            }
        }
    }

    ParsedBlock result{std::string(source), std::move(identifier),
                       std::move(type), {}};
    for (const auto& [slot, path] : bindings) result.texturePaths.push_back(path);
    std::sort(result.texturePaths.begin(), result.texturePaths.end());
    result.texturePaths.erase(
        std::unique(result.texturePaths.begin(), result.texturePaths.end()),
        result.texturePaths.end());
    return result;
}

} // namespace

BlockLoadReport BlockLoader::loadFromManifest(
    Asset::AssetManager& assets,
    BlockModelRegistry& models,
    BlockRegistry& registry,
    TextureAtlas& atlas
) {
    std::vector<BlockModelDefinitionSource> modelDefinitions;
    std::vector<BlockDefinitionSource> blockDefinitions;
    for (const std::string_view path : ResourceRegistry::Paths()) {
        if (!endsWith(path, ".yaml")) continue;
        if (startsWith(path, "models/blocks/")) {
            modelDefinitions.push_back(
                {path, ResourceRegistry::Get(std::string(path))});
        } else if (startsWith(path, "blocks/")) {
            blockDefinitions.push_back(
                {path, ResourceRegistry::Get(std::string(path))});
        }
    }
    return loadDefinitions(
        assets.ns(), modelDefinitions, blockDefinitions, models, registry, atlas);
}

BlockLoadReport BlockLoader::loadDefinitions(
    std::string_view assetNamespace,
    std::span<const BlockModelDefinitionSource> modelDefinitions,
    std::span<const BlockDefinitionSource> definitions,
    BlockModelRegistry& models,
    BlockRegistry& registry,
    TextureAtlas& atlas
) {
    BlockLoadReport report;
    report.modelsDiscovered = modelDefinitions.size();
    report.discovered = definitions.size();

    std::vector<BlockModelDefinitionSource> orderedModels(
        modelDefinitions.begin(), modelDefinitions.end());
    std::sort(orderedModels.begin(), orderedModels.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    std::vector<std::shared_ptr<const BlockModel>> stagedModels;
    std::unordered_map<std::string, std::shared_ptr<const BlockModel>> stagedLookup;
    for (const auto& definition : orderedModels) {
        try {
            auto model = parseModel(
                definition.path, definition.data, assetNamespace);
            if (models.contains(model->identifier()) ||
                !stagedLookup.emplace(model->identifier(), model).second) {
                throw std::invalid_argument(
                    "Duplicate normalized block model ID '" +
                    model->identifier() + "'");
            }
            stagedModels.push_back(std::move(model));
        } catch (const std::exception& error) {
            ++report.modelsFailed;
            addFailure(report, definition.path, error.what());
        }
    }

    if (report.modelsFailed != 0) {
        report.skipped = definitions.size();
        return report;
    }

    std::vector<BlockDefinitionSource> orderedDefinitions(
        definitions.begin(), definitions.end());
    std::sort(orderedDefinitions.begin(), orderedDefinitions.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    std::vector<ParsedBlock> stagedBlocks;
    std::unordered_set<std::string> stagedBlockIds;
    for (const auto& definition : orderedDefinitions) {
        try {
            ParsedBlock block = parseBlock(
                definition.path, definition.data, assetNamespace,
                stagedLookup, models);
            if (registry.hasIdentifier(block.identifier) ||
                !stagedBlockIds.insert(block.identifier).second) {
                ++report.skipped;
                continue;
            }
            stagedBlocks.push_back(std::move(block));
        } catch (const std::exception& error) {
            ++report.failed;
            addFailure(report, definition.path, error.what());
        }
    }

    if (report.failed != 0) {
        report.skipped += stagedBlocks.size();
        return report;
    }

    BlockModelRegistry preparedModels = models;
    BlockRegistry preparedRegistry = registry;
    preparedModels.registerModels(stagedModels);
    std::vector<std::pair<std::string, BlockType>> registrations;
    registrations.reserve(stagedBlocks.size());
    for (ParsedBlock& block : stagedBlocks) {
        registrations.emplace_back(
            block.identifier, std::move(block.type));
    }
    preparedRegistry.registerBlocks(std::move(registrations));

    const size_t originalTextureCount = atlas.textureCount();
    try {
        for (const ParsedBlock& block : stagedBlocks) {
            try {
                for (const std::string& path : block.texturePaths) {
                    atlas.addTextureFromResource(path);
                }
            } catch (const std::exception& error) {
                ++report.failed;
                addFailure(report, block.source, error.what());
            }
        }
    } catch (...) {
        atlas.rollbackTo(originalTextureCount);
        throw;
    }
    if (report.failed != 0) {
        atlas.rollbackTo(originalTextureCount);
        report.skipped += stagedBlocks.size() - report.failed;
        return report;
    }

    report.modelsLoaded = stagedModels.size();
    report.loaded = stagedBlocks.size();

    try {
        spdlog::info(
            "Block assets: {} models and {} blocks loaded ({} model failures, "
            "{} block failures, {} blocks skipped)",
            report.modelsLoaded, report.loaded, report.modelsFailed,
            report.failed, report.skipped);
    } catch (...) {
        atlas.rollbackTo(originalTextureCount);
        throw;
    }
    models.swap(preparedModels);
    registry.swap(preparedRegistry);
    return report;
}

BlockLoadReport BlockLoader::loadDefinitions(
    std::string_view assetNamespace,
    std::span<const BlockDefinitionSource> definitions,
    BlockRegistry& registry,
    TextureAtlas& atlas
) {
    BlockModelRegistry models;
    return loadDefinitions(
        assetNamespace, std::span<const BlockModelDefinitionSource>{},
        definitions, models, registry, atlas);
}

} // namespace Rigel::Voxel
