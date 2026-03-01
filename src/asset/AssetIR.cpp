#include "Rigel/Asset/AssetIR.h"
#include "ResourceRegistry.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace Rigel::Asset::IR {

namespace {

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool hasAnySuffix(std::string_view value, std::initializer_list<std::string_view> suffixes) {
    for (std::string_view suffix : suffixes) {
        if (endsWith(value, suffix)) {
            return true;
        }
    }
    return false;
}

std::string stripVariantSuffix(const std::string& id) {
    size_t bracketPos = id.find('[');
    if (bracketPos == std::string::npos) {
        return id;
    }
    return id.substr(0, bracketPos);
}

std::string removeSuffix(std::string_view path, std::string_view suffix) {
    if (!endsWith(path, suffix)) {
        return std::string(path);
    }
    return std::string(path.substr(0, path.size() - suffix.size()));
}

std::optional<std::string> nodeString(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string out;
    node[ryml::to_csubstr(key)] >> out;
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

std::optional<bool> nodeBool(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string out;
    node[ryml::to_csubstr(key)] >> out;
    if (out == "true" || out == "yes" || out == "1") {
        return true;
    }
    if (out == "false" || out == "no" || out == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<uint8_t> nodeU8(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string out;
    node[ryml::to_csubstr(key)] >> out;
    try {
        int parsed = std::stoi(out);
        if (parsed < 0 || parsed > 255) {
            return std::nullopt;
        }
        return static_cast<uint8_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::string toGenericPath(const std::filesystem::path& path) {
    return path.generic_string();
}

void sortUnique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string readFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Unable to read file: " + toGenericPath(path));
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<std::string> extractStringIds(const std::string& text) {
    static const std::regex stringIdPattern(R"re("stringId"\s*:\s*"([^"]+)")re");
    std::vector<std::string> ids;
    for (std::sregex_iterator it(text.begin(), text.end(), stringIdPattern), end;
         it != end;
         ++it) {
        if ((*it).size() >= 2) {
            ids.push_back((*it)[1].str());
        }
    }
    return ids;
}

std::string rigelManifestNamespace() {
    try {
        auto data = ResourceRegistry::Get("manifest.yaml");
        ryml::Tree tree = ryml::parse_in_arena(
            ryml::to_csubstr("manifest.yaml"),
            ryml::csubstr(data.data(), data.size())
        );
        ryml::ConstNodeRef root = tree.rootref();
        if (auto ns = nodeString(root, "namespace")) {
            return *ns;
        }
    } catch (const std::exception&) {
        // fallback below
    }
    return "base";
}

std::string blockIdFromEmbeddedPath(const std::string& path,
                                    ryml::ConstNodeRef root,
                                    const std::string& manifestNs) {
    std::string id;
    if (auto explicitId = nodeString(root, "id")) {
        id = *explicitId;
    } else if (auto explicitIdentifier = nodeString(root, "identifier")) {
        id = *explicitIdentifier;
    } else if (auto explicitName = nodeString(root, "name")) {
        id = *explicitName;
    } else {
        std::string_view relative(path);
        if (startsWith(relative, "blocks/")) {
            relative.remove_prefix(std::string_view("blocks/").size());
        }
        id = removeSuffix(relative, ".yaml");
    }

    if (id.find(':') == std::string::npos && !manifestNs.empty()) {
        id = manifestNs + ":" + id;
    }
    return id;
}

void collectFilesystemPaths(const std::filesystem::path& root,
                            std::vector<std::filesystem::path>& out,
                            std::initializer_list<std::string_view> suffixes) {
    out.clear();
    if (!std::filesystem::exists(root)) {
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string path = toGenericPath(entry.path());
        if (!hasAnySuffix(path, suffixes)) {
            continue;
        }
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return toGenericPath(a) < toGenericPath(b);
    });
}

std::string relativeOrAbsolute(const std::filesystem::path& path,
                               const std::filesystem::path& base) {
    std::error_code ec;
    auto relative = std::filesystem::relative(path, base, ec);
    if (!ec) {
        return toGenericPath(relative);
    }
    return toGenericPath(path);
}

template <typename T>
void sortByIdentifier(std::vector<T>& values) {
    std::sort(values.begin(), values.end(), [](const T& a, const T& b) {
        if (a.identifier == b.identifier) {
            return a.sourcePath < b.sourcePath;
        }
        return a.identifier < b.identifier;
    });
}

} // namespace

AssetGraphIR compileRigelEmbedded() {
    AssetGraphIR graph;
    std::string ns = rigelManifestNamespace();

    std::vector<std::string> blockPaths;
    for (std::string_view path : ResourceRegistry::Paths()) {
        if (startsWith(path, "blocks/") && endsWith(path, ".yaml")) {
            blockPaths.emplace_back(path);
        }
    }
    std::sort(blockPaths.begin(), blockPaths.end());

    std::unordered_map<std::string, size_t> rootToIndex;
    for (const std::string& path : blockPaths) {
        auto data = ResourceRegistry::Get(path);
        ryml::Tree tree = ryml::parse_in_arena(
            ryml::to_csubstr(path.c_str()),
            ryml::csubstr(data.data(), data.size())
        );
        ryml::ConstNodeRef root = tree.rootref();

        BlockStateIR state;
        state.identifier = blockIdFromEmbeddedPath(path, root, ns);
        state.rootIdentifier = stripVariantSuffix(state.identifier);
        state.sourcePath = path;

        if (auto model = nodeString(root, "model")) {
            state.model = *model;
        }
        if (auto layer = nodeString(root, "layer")) {
            state.renderLayer = *layer;
        }
        if (auto opaque = nodeBool(root, "opaque")) {
            state.isOpaque = *opaque;
        }
        if (auto solid = nodeBool(root, "solid")) {
            state.isSolid = *solid;
        }
        if (auto cullSameType = nodeBool(root, "cull_same_type")) {
            state.cullSameType = *cullSameType;
        }
        if (auto emitted = nodeU8(root, "emits_light")) {
            state.emittedLight = *emitted;
        }
        if (auto attenuation = nodeU8(root, "light_attenuation")) {
            state.lightAttenuation = *attenuation;
        }

        static const std::set<std::string> knownRootFields = {
            "id", "identifier", "name", "model", "layer", "opaque", "solid",
            "cull_same_type", "emits_light", "light_attenuation", "textures"
        };
        for (ryml::ConstNodeRef child : root.children()) {
            std::string key(child.key().data(), child.key().size());
            if (knownRootFields.find(key) != knownRootFields.end()) {
                continue;
            }
            if (child.has_val()) {
                std::string value;
                child >> value;
                state.extensions[key] = value;
            }
        }

        if (root.has_child("textures")) {
            ryml::ConstNodeRef textures = root["textures"];
            for (ryml::ConstNodeRef texNode : textures.children()) {
                std::string key(texNode.key().data(), texNode.key().size());
                std::string value;
                texNode >> value;
                state.textures[key] = value;
            }
        }

        auto it = rootToIndex.find(state.rootIdentifier);
        if (it == rootToIndex.end()) {
            BlockDefIR blockDef;
            blockDef.rootIdentifier = state.rootIdentifier;
            blockDef.sourcePath = state.sourcePath;
            blockDef.states.push_back(std::move(state));
            graph.blocks.push_back(std::move(blockDef));
            rootToIndex.emplace(graph.blocks.back().rootIdentifier, graph.blocks.size() - 1);
        } else {
            graph.blocks[it->second].states.push_back(std::move(state));
        }
    }

    for (auto& block : graph.blocks) {
        std::sort(block.states.begin(), block.states.end(), [](const BlockStateIR& a, const BlockStateIR& b) {
            return a.identifier < b.identifier;
        });
    }
    std::sort(graph.blocks.begin(), graph.blocks.end(), [](const BlockDefIR& a, const BlockDefIR& b) {
        return a.rootIdentifier < b.rootIdentifier;
    });

    for (std::string_view path : ResourceRegistry::Paths()) {
        if (startsWith(path, "models/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.models.push_back(ModelRefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "materials/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.materials.push_back(MaterialRefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "models/entities/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.entities.push_back(EntityDefIR{std::string(path), std::string(path), {}});
        } else if (startsWith(path, "items/") && hasAnySuffix(path, {".json", ".yaml", ".yml"})) {
            graph.items.push_back(ItemDefIR{std::string(path), std::string(path), {}});
        }
    }
    sortByIdentifier(graph.models);
    sortByIdentifier(graph.materials);
    sortByIdentifier(graph.entities);
    sortByIdentifier(graph.items);

    return graph;
}

AssetGraphIR compileCRFilesystem(const std::filesystem::path& root) {
    AssetGraphIR graph;

    std::filesystem::path baseRoot = root;
    if (std::filesystem::exists(root / "base")) {
        baseRoot = root / "base";
    }

    std::vector<std::filesystem::path> blockFiles;
    collectFilesystemPaths(baseRoot / "blocks", blockFiles, {".json"});
    std::unordered_map<std::string, size_t> rootToIndex;
    for (const auto& file : blockFiles) {
        std::string text = readFileText(file);
        std::vector<std::string> ids = extractStringIds(text);
        if (ids.empty()) {
            ids.push_back("base:" + file.stem().string());
        }
        for (const std::string& id : ids) {
            BlockStateIR state;
            state.identifier = id;
            state.rootIdentifier = stripVariantSuffix(id);
            state.sourcePath = relativeOrAbsolute(file, baseRoot);
            state.extensions["source_format"] = "cr";

            auto it = rootToIndex.find(state.rootIdentifier);
            if (it == rootToIndex.end()) {
                BlockDefIR def;
                def.rootIdentifier = state.rootIdentifier;
                def.sourcePath = state.sourcePath;
                def.states.push_back(std::move(state));
                graph.blocks.push_back(std::move(def));
                rootToIndex.emplace(graph.blocks.back().rootIdentifier, graph.blocks.size() - 1);
            } else {
                graph.blocks[it->second].states.push_back(std::move(state));
            }
        }
    }

    std::sort(graph.blocks.begin(), graph.blocks.end(), [](const BlockDefIR& a, const BlockDefIR& b) {
        return a.rootIdentifier < b.rootIdentifier;
    });
    for (auto& block : graph.blocks) {
        std::sort(block.states.begin(), block.states.end(), [](const BlockStateIR& a, const BlockStateIR& b) {
            return a.identifier < b.identifier;
        });
    }

    std::vector<std::filesystem::path> files;
    collectFilesystemPaths(baseRoot / "models", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.models.push_back(ModelRefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "materials", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.materials.push_back(MaterialRefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "entities", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.entities.push_back(EntityDefIR{id, id, {{"source_format", "cr"}}});
    }
    collectFilesystemPaths(baseRoot / "items", files, {".json", ".yaml", ".yml"});
    for (const auto& file : files) {
        std::string id = relativeOrAbsolute(file, baseRoot);
        graph.items.push_back(ItemDefIR{id, id, {{"source_format", "cr"}}});
    }
    sortByIdentifier(graph.models);
    sortByIdentifier(graph.materials);
    sortByIdentifier(graph.entities);
    sortByIdentifier(graph.items);

    return graph;
}

std::vector<ValidationIssue> validate(const AssetGraphIR& graph) {
    std::vector<ValidationIssue> issues;

    auto add = [&](ValidationSeverity severity,
                   std::string sourcePath,
                   std::string identifier,
                   std::string field,
                   std::string message) {
        issues.push_back(ValidationIssue{
            .severity = severity,
            .sourcePath = std::move(sourcePath),
            .identifier = std::move(identifier),
            .field = std::move(field),
            .message = std::move(message)
        });
    };

    std::unordered_map<std::string, std::string> blockVariantOwners;
    for (const auto& block : graph.blocks) {
        if (block.rootIdentifier.empty()) {
            add(ValidationSeverity::Error, block.sourcePath, "", "rootIdentifier",
                "Block root identifier is empty");
        }
        if (block.states.empty()) {
            add(ValidationSeverity::Warning, block.sourcePath, block.rootIdentifier, "states",
                "Block definition has no states");
        }
        for (const auto& state : block.states) {
            if (state.identifier.empty()) {
                add(ValidationSeverity::Error, state.sourcePath, "", "identifier",
                    "Block state identifier is empty");
                continue;
            }
            auto [it, inserted] = blockVariantOwners.emplace(state.identifier, state.sourcePath);
            if (!inserted) {
                add(ValidationSeverity::Error, state.sourcePath, state.identifier, "identifier",
                    "Duplicate block state identifier also seen in " + it->second);
            }
            std::string expectedRoot = stripVariantSuffix(state.identifier);
            if (expectedRoot != state.rootIdentifier) {
                add(ValidationSeverity::Error, state.sourcePath, state.identifier, "rootIdentifier",
                    "State root '" + state.rootIdentifier + "' does not match identifier-derived root '" +
                        expectedRoot + "'");
            }
            if (state.renderLayer != "opaque" && state.renderLayer != "cutout" &&
                state.renderLayer != "transparent" && state.renderLayer != "emissive") {
                add(ValidationSeverity::Warning, state.sourcePath, state.identifier, "renderLayer",
                    "Unknown render layer '" + state.renderLayer + "'");
            }
            if (state.model == "cube" && state.textures.empty()) {
                add(ValidationSeverity::Warning, state.sourcePath, state.identifier, "textures",
                    "Cube model has no textures configured");
            }
        }
    }

    auto checkDuplicateSimpleIds = [&](const auto& values, const char* fieldName) {
        std::unordered_map<std::string, std::string> owners;
        for (const auto& value : values) {
            if (value.identifier.empty()) {
                add(ValidationSeverity::Error, value.sourcePath, "", fieldName, "Identifier is empty");
                continue;
            }
            auto [it, inserted] = owners.emplace(value.identifier, value.sourcePath);
            if (!inserted) {
                add(ValidationSeverity::Error, value.sourcePath, value.identifier, fieldName,
                    std::string("Duplicate identifier also seen in ") + it->second);
            }
        }
    };

    checkDuplicateSimpleIds(graph.models, "model.identifier");
    checkDuplicateSimpleIds(graph.materials, "material.identifier");
    checkDuplicateSimpleIds(graph.entities, "entity.identifier");
    checkDuplicateSimpleIds(graph.items, "item.identifier");

    return issues;
}

} // namespace Rigel::Asset::IR

