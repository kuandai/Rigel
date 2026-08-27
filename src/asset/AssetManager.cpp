#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/RawLoader.h"
#include "Rigel/Asset/TextureLoader.h"
#include "Rigel/Asset/ShaderLoader.h"
#include "ResourceRegistry.h"
#include "Rigel/Util/Ryml.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace Rigel::Asset {
namespace {

class ManifestYamlParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throwManifestYamlParseError(const char* message,
                                              size_t length,
                                              ryml::Location,
                                              void*) {
    throw ManifestYamlParseError(std::string(message, length));
}

ryml::Tree parseManifestYaml(std::span<const char> data,
                             const std::string& path) {
    ryml::Callbacks callbacks = ryml::get_callbacks();
    callbacks.m_error = &throwManifestYamlParseError;
    ryml::Tree tree(callbacks);
    ryml::Parser::handler_type handler(callbacks);
    ryml::Parser parser(&handler);
    try {
        ryml::parse_in_arena(
            &parser,
            ryml::to_csubstr(path.c_str()),
            ryml::csubstr(data.data(), data.size()),
            &tree);
    } catch (const ManifestYamlParseError& error) {
        throw AssetLoadError(path, "invalid manifest YAML: " +
                                       std::string(error.what()));
    }
    return tree;
}

AssetManager::AssetEntry cloneManifestEntry(
    ryml::ConstNodeRef assetNode,
    const std::string& categoryName) {
    AssetManager::AssetEntry entry;
    entry.category = categoryName;

    ryml::Tree subtree;
    subtree.reserve(assetNode.num_children() * 2 + 1);
    size_t arenaCapacity = 0;
    for (ryml::ConstNodeRef child : assetNode.children()) {
        arenaCapacity += child.key().size();
        if (child.has_val()) {
            arenaCapacity += child.val().size();
        }
        for (ryml::ConstNodeRef grandchild : child.children()) {
            if (grandchild.has_key()) {
                arenaCapacity += grandchild.key().size();
            }
            if (grandchild.has_val()) {
                arenaCapacity += grandchild.val().size();
            }
        }
    }
    subtree.reserve_arena(arenaCapacity);

    ryml::NodeRef subtreeRoot = subtree.rootref();
    subtreeRoot |= ryml::MAP;
    for (ryml::ConstNodeRef child : assetNode.children()) {
        const ryml::csubstr key = subtree.copy_to_arena(child.key());
        if (child.is_keyval()) {
            subtreeRoot[key] = subtree.copy_to_arena(child.val());
        } else if (child.has_children()) {
            ryml::NodeRef newChild = subtreeRoot.append_child();
            newChild.set_key(key);
            if (child.is_map()) {
                newChild |= ryml::MAP;
            } else if (child.is_seq()) {
                newChild |= ryml::SEQ;
            }
            for (ryml::ConstNodeRef grandchild : child.children()) {
                ryml::NodeRef copy = newChild.append_child();
                if (grandchild.has_key()) {
                    copy.set_key(subtree.copy_to_arena(grandchild.key()));
                }
                if (grandchild.has_val()) {
                    copy.set_val(subtree.copy_to_arena(grandchild.val()));
                }
            }
        }
    }

    entry.configTree = std::move(subtree);
    entry.config = entry.configTree.rootref();
    return entry;
}

void validateGeneratorDeclaration(ryml::ConstNodeRef assetNode,
                                  const std::string& fullId) {
    if (!assetNode.is_map()) {
        throw AssetLoadError(
            fullId, "generator definition declaration must be a mapping");
    }
    bool foundPath = false;
    std::unordered_set<std::string> fields;
    for (ryml::ConstNodeRef field : assetNode.children()) {
        const std::string fieldName = Util::toStdString(field.key());
        if (!fields.insert(fieldName).second) {
            throw AssetLoadError(
                fullId,
                "duplicate generator definition declaration field '" +
                    fieldName + "'");
        }
        if (fieldName != "path") {
            throw AssetLoadError(
                fullId,
                "unknown generator definition declaration field '" +
                    fieldName + "'");
        }
        if (!field.has_val() || field.has_children() || field.val().empty()) {
            throw AssetLoadError(
                fullId,
                "generator definition declaration path must be a non-empty scalar");
        }
        foundPath = true;
    }
    if (!foundPath) {
        throw AssetLoadError(
            fullId,
            "generator definition declaration requires a non-empty path");
    }
}

} // namespace

// AssetEntry convenience methods
std::optional<std::string> AssetManager::AssetEntry::getString(const std::string& key) const {
    ryml::ConstNodeRef root = configTree.crootref();
    if (!root.readable() || !root.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    const ryml::ConstNodeRef node = root[ryml::to_csubstr(key)];
    if (!node.has_val() || node.has_children()) {
        return std::nullopt;
    }
    std::string value;
    node >> value;
    return value;
}

// LoadContext implementation
std::span<const char> LoadContext::loadResource(const std::string& path) const {
    return ResourceRegistry::Get(path);
}

void AssetManager::loadManifest(const std::string& path) {
    spdlog::info("Loading asset manifest: {}", path);

    // Register each built-in loader unless that category was explicitly replaced.
    if (!m_loaders.contains("raw")) {
        registerLoader("raw", std::make_unique<RawLoader>());
    }
    if (!m_loaders.contains("textures")) {
        registerLoader("textures", std::make_unique<TextureLoader>());
    }
    if (!m_loaders.contains("shaders")) {
        registerLoader("shaders", std::make_unique<ShaderLoader>());
    }

    // Get raw manifest data from embedded resources
    auto data = ResourceRegistry::Get(path);

    // Parse YAML
    ryml::Tree tree = parseManifestYaml(data, path);
    ryml::ConstNodeRef root = tree.rootref();

    // Parse the namespace without publishing it until exact declarations pass.
    std::optional<std::string> manifestNamespace;
    if (root.has_child("namespace")) {
        manifestNamespace.emplace();
        root["namespace"] >> *manifestNamespace;
    }

    // Parse assets
    if (!root.has_child("assets")) {
        if (manifestNamespace) {
            m_namespace = std::move(*manifestNamespace);
            if (m_pendingGeneratorDefinitions) {
                m_pendingGeneratorDefinitions->previousNamespace = m_namespace;
            }
            spdlog::debug("Manifest namespace: {}", m_namespace);
        }
        spdlog::warn("Manifest has no 'assets' section");
        return;
    }

    ryml::ConstNodeRef assets = root["assets"];

    // A manifest that declares generator definitions remains provisional until
    // the complete installed set and the requested definition are validated.
    // Declaration-shape failures are retained privately so published worlds can
    // continue using their save-owned snapshot without resolving installed
    // content.
    std::optional<PendingGeneratorDefinitions> generatorCandidate;
    bool foundGeneratorCategory = false;
    for (const ryml::ConstNodeRef category : assets.children()) {
        const std::string categoryName = Util::toStdString(category.key());
        if (categoryName != "generator_definitions") {
            continue;
        }
        if (!generatorCandidate) {
            generatorCandidate.emplace();
        }
        try {
            if (foundGeneratorCategory) {
                throw AssetLoadError(
                    "generator_definitions",
                    "duplicate generator definition manifest category");
            }
            foundGeneratorCategory = true;
            if (!category.is_map()) {
                throw AssetLoadError(
                    "generator_definitions",
                    "generator definition manifest category must be a mapping");
            }
            for (const ryml::ConstNodeRef assetNode : category.children()) {
                const std::string fullId = categoryName + "/" +
                    Util::toStdString(assetNode.key());
                validateGeneratorDeclaration(assetNode, fullId);
                auto [it, inserted] = generatorCandidate->entries.emplace(
                    fullId, cloneManifestEntry(assetNode, categoryName));
                if (!inserted) {
                    throw AssetLoadError(
                        fullId,
                        "duplicate generator definition asset declaration");
                }
                it->second.config = it->second.configTree.rootref();
            }
        } catch (const AssetLoadError&) {
            if (!generatorCandidate->declarationError) {
                generatorCandidate->declarationError =
                    std::current_exception();
            }
        }
    }

    // Clone ordinary entries before publishing any part of this manifest.
    std::unordered_map<std::string, AssetEntry> manifestEntries;
    for (ryml::ConstNodeRef category : assets.children()) {
        std::string categoryName = Util::toStdString(category.key());
        if (categoryName == "generator_definitions") {
            continue;
        }

        // Iterate assets in category
        for (ryml::ConstNodeRef assetNode : category.children()) {
            std::string assetName = Util::toStdString(assetNode.key());
            const std::string fullId = categoryName + "/" + assetName;

            AssetEntry entry = cloneManifestEntry(assetNode, categoryName);

            // Log the path if present, or note that it's a complex asset
            auto pathOpt = entry.getString("path");
            if (pathOpt) {
                spdlog::debug("Registered asset: {} -> {}", fullId, *pathOpt);
            } else {
                spdlog::debug("Registered asset: {} (complex config)", fullId);
            }

            manifestEntries[fullId] = std::move(entry);
            manifestEntries[fullId].config =
                manifestEntries[fullId].configTree.rootref();
        }
    }

    auto startsWith = [](std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() &&
               value.compare(0, prefix.size(), prefix) == 0;
    };

    auto endsWith = [](std::string_view value, std::string_view suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    auto readEmbeddedId = [&](const std::string& entryPath) -> std::optional<std::string> {
        if (!endsWith(entryPath, ".json") && !endsWith(entryPath, ".yaml") &&
            !endsWith(entryPath, ".yml")) {
            return std::nullopt;
        }
        try {
            auto data = ResourceRegistry::Get(entryPath);
            ryml::Tree tree = ryml::parse_in_arena(
                ryml::to_csubstr(entryPath.c_str()),
                ryml::csubstr(data.data(), data.size())
            );
            ryml::ConstNodeRef root = tree.rootref();
            if (root.has_child("id")) {
                std::string id;
                root["id"] >> id;
                if (!id.empty()) {
                    return id;
                }
            }
            if (root.has_child("name")) {
                std::string name;
                root["name"] >> name;
                if (!name.empty()) {
                    return name;
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("AssetManager: failed to read id from '{}': {}", entryPath, e.what());
        }
        return std::nullopt;
    };

    auto registerEmbeddedCategory = [&](const std::string& category,
                                        std::string_view prefix,
                                        std::string_view primarySuffix,
                                        std::string_view secondarySuffix = {},
                                        bool stripSuffix = true) {
        std::vector<std::string> paths;
        for (std::string_view pathView : ResourceRegistry::Paths()) {
            if (!startsWith(pathView, prefix)) {
                continue;
            }
            if (!endsWith(pathView, primarySuffix) &&
                (secondarySuffix.empty() || !endsWith(pathView, secondarySuffix))) {
                continue;
            }
            paths.emplace_back(pathView);
        }
        std::sort(paths.begin(), paths.end());

        for (const auto& entryPath : paths) {
            std::string name;
            if (auto id = readEmbeddedId(entryPath)) {
                name = *id;
            } else {
                std::string_view relative(entryPath);
                relative.remove_prefix(prefix.size());
            if (stripSuffix) {
                if (endsWith(relative, ".animation.json")) {
                    relative.remove_suffix(std::string_view(".animation.json").size());
                } else if (endsWith(relative, primarySuffix)) {
                    relative.remove_suffix(primarySuffix.size());
                } else if (!secondarySuffix.empty() && endsWith(relative, secondarySuffix)) {
                    relative.remove_suffix(secondarySuffix.size());
                }
            }
            name.assign(relative);
        }

            if (name.empty()) {
                spdlog::warn("AssetManager: skipped embedded {} entry with empty id ('{}')",
                             category, entryPath);
                continue;
            }

            std::string fullId = category + "/" + name;
            const auto existedBeforePending = [&] {
                if (!m_pendingGeneratorDefinitions ||
                    !m_pendingGeneratorDefinitions->touchedEntryIds.contains(
                        fullId)) {
                    return m_entries.contains(fullId);
                }
                return m_pendingGeneratorDefinitions->previousEntries.contains(
                    fullId);
            }();
            if (manifestEntries.contains(fullId) || existedBeforePending) {
                continue;
            }

            AssetEntry entry;
            entry.category = category;
            ryml::Tree tree;
            ryml::NodeRef root = tree.rootref();
            root |= ryml::MAP;
            root[ryml::to_csubstr("path")] =
                tree.copy_to_arena(ryml::to_csubstr(entryPath));
            entry.configTree = std::move(tree);
            entry.config = entry.configTree.rootref();
            auto [it, inserted] = manifestEntries.emplace(
                std::move(fullId), std::move(entry));
            if (inserted) {
                it->second.config = it->second.configTree.rootref();
            }
        }
    };

    registerEmbeddedCategory("entity_models", "models/entities/", ".json", ".yaml", true);
    registerEmbeddedCategory("entity_anims", "animations/entities/", ".json", ".yaml", true);
    registerEmbeddedCategory("textures", "textures/", ".png", {}, false);

    if (generatorCandidate) {
        // A corrected generator manifest replaces any unresolved candidate and
        // starts again from the last committed manifest state.
        discardPendingGeneratorDefinitions();

        generatorCandidate->previousNamespace = m_namespace;
        for (const auto& [id, entry] : manifestEntries) {
            static_cast<void>(entry);
            generatorCandidate->touchedEntryIds.insert(id);
        }
        for (const auto& [key, asset] : m_cache) {
            if (generatorCandidate->touchedEntryIds.contains(key.second)) {
                generatorCandidate->previousCacheEntries.emplace(key, asset);
            }
        }
        if (manifestNamespace) {
            m_namespace = std::move(*manifestNamespace);
            spdlog::debug("Manifest namespace: {}", m_namespace);
        }
        for (auto& [id, entry] : manifestEntries) {
            if (auto found = m_entries.find(id); found != m_entries.end()) {
                generatorCandidate->previousEntries.emplace(
                    id, std::move(found->second));
            }
            m_entries[id] = std::move(entry);
            m_entries[id].config = m_entries[id].configTree.rootref();
        }
        m_pendingGeneratorDefinitions = std::move(*generatorCandidate);
    } else {
        if (manifestNamespace) {
            m_namespace = std::move(*manifestNamespace);
            if (m_pendingGeneratorDefinitions) {
                m_pendingGeneratorDefinitions->previousNamespace = m_namespace;
            }
            spdlog::debug("Manifest namespace: {}", m_namespace);
        }
        if (m_pendingGeneratorDefinitions) {
            for (const auto& [id, entry] : manifestEntries) {
                static_cast<void>(entry);
                m_pendingGeneratorDefinitions->touchedEntryIds.erase(id);
                m_pendingGeneratorDefinitions->previousEntries.erase(id);
                std::erase_if(
                    m_pendingGeneratorDefinitions->previousCacheEntries,
                    [&](const auto& item) {
                        return item.first.second == id;
                    });
            }
        }
        for (auto& [id, entry] : manifestEntries) {
            m_entries[id] = std::move(entry);
            m_entries[id].config = m_entries[id].configTree.rootref();
        }
    }

    for (auto& [id, entry] : m_entries) {
        static_cast<void>(id);
        entry.config = entry.configTree.rootref();
    }

    spdlog::info("Loaded {} assets from manifest", m_entries.size());
}

bool AssetManager::exists(const std::string& id) const {
    return m_entries.find(id) != m_entries.end();
}

void AssetManager::clearCache() {
    m_cache.clear();
    spdlog::debug("Asset cache cleared");
}

void AssetManager::registerLoader(const std::string& category, std::unique_ptr<IAssetLoader> loader) {
    spdlog::debug("Registered loader for category: {}", category);
    m_loaders[category] = std::move(loader);
}

void AssetManager::forEachInCategory(
    const std::string& category,
    const std::function<void(const std::string& name, const AssetEntry& entry)>& fn
) const {
    std::string prefix = category + "/";

    for (const auto& [id, entry] : m_entries) {
        if (entry.category == category) {
            // Extract the name part (everything after "category/")
            std::string name = id.substr(prefix.length());
            fn(name, entry);
        }
    }
}

bool AssetManager::hasPendingGeneratorDefinitions() const {
    return m_pendingGeneratorDefinitions.has_value();
}

void AssetManager::forEachGeneratorDefinitionCandidate(
    const std::function<void(const std::string& name, const AssetEntry& entry)>& fn
) const {
    if (!m_pendingGeneratorDefinitions) {
        forEachInCategory("generator_definitions", fn);
        return;
    }
    const std::string prefix = "generator_definitions/";
    for (const auto& [id, entry] : m_pendingGeneratorDefinitions->entries) {
        fn(id.substr(prefix.size()), entry);
    }
}

void AssetManager::rethrowPendingGeneratorDefinitionError() const {
    if (m_pendingGeneratorDefinitions &&
        m_pendingGeneratorDefinitions->declarationError) {
        std::rethrow_exception(
            m_pendingGeneratorDefinitions->declarationError);
    }
}

void AssetManager::commitPendingGeneratorDefinitions() {
    if (!m_pendingGeneratorDefinitions) {
        return;
    }
    std::erase_if(m_entries, [](const auto& item) {
        return item.second.category == "generator_definitions";
    });
    for (auto& [id, entry] : m_pendingGeneratorDefinitions->entries) {
        auto [it, inserted] = m_entries.emplace(id, std::move(entry));
        static_cast<void>(inserted);
        it->second.config = it->second.configTree.rootref();
    }
    std::erase_if(m_cache, [](const auto& item) {
        return item.first.second.starts_with("generator_definitions/");
    });
    std::erase_if(m_cache, [&](const auto& item) {
        return m_pendingGeneratorDefinitions->touchedEntryIds.contains(
            item.first.second);
    });
    m_pendingGeneratorDefinitions.reset();
}

void AssetManager::discardPendingGeneratorDefinitions() {
    if (!m_pendingGeneratorDefinitions) {
        return;
    }

    m_namespace = std::move(
        m_pendingGeneratorDefinitions->previousNamespace);
    for (const std::string& id :
         m_pendingGeneratorDefinitions->touchedEntryIds) {
        m_entries.erase(id);
    }
    for (auto& [id, entry] :
         m_pendingGeneratorDefinitions->previousEntries) {
        m_entries[id] = std::move(entry);
        m_entries[id].config = m_entries[id].configTree.rootref();
    }
    std::erase_if(m_cache, [&](const auto& item) {
        return m_pendingGeneratorDefinitions->touchedEntryIds.contains(
            item.first.second);
    });
    for (auto& [key, asset] :
         m_pendingGeneratorDefinitions->previousCacheEntries) {
        m_cache.emplace(std::move(key), std::move(asset));
    }
    m_pendingGeneratorDefinitions.reset();
}

} // namespace Rigel::Asset
