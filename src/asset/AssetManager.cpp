#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Asset/RawLoader.h"
#include "Rigel/Asset/TextureLoader.h"
#include "Rigel/Asset/ShaderLoader.h"
#include "ResourceRegistry.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Rigel::Asset {

namespace {
    // Helper to convert csubstr to std::string
    std::string toStdString(ryml::csubstr s) {
        return std::string(s.data(), s.size());
    }
}

// AssetEntry convenience methods
std::optional<std::string> AssetManager::AssetEntry::getString(const std::string& key) const {
    if (!config.readable() || !config.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string value;
    config[ryml::to_csubstr(key)] >> value;
    return value;
}

bool AssetManager::AssetEntry::hasChild(const std::string& key) const {
    return config.readable() && config.has_child(ryml::to_csubstr(key));
}

// LoadContext implementation
std::span<const char> LoadContext::loadResource(const std::string& path) const {
    return ResourceRegistry::Get(path);
}

void AssetManager::loadManifest(const std::string& path) {
    spdlog::info("Loading asset manifest: {}", path);

    // Register built-in loaders if not already registered
    if (m_loaders.empty()) {
        registerLoader("raw", std::make_unique<RawLoader>());
        registerLoader("textures", std::make_unique<TextureLoader>());
        registerLoader("shaders", std::make_unique<ShaderLoader>());
    }

    // Get raw manifest data from embedded resources
    auto data = ResourceRegistry::Get(path);

    // Parse YAML
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(path.c_str()),
        ryml::csubstr(data.data(), data.size())
    );
    ryml::ConstNodeRef root = tree.rootref();

    // Extract namespace
    if (root.has_child("namespace")) {
        root["namespace"] >> m_namespace;
        spdlog::debug("Manifest namespace: {}", m_namespace);
    }

    // Parse assets
    if (!root.has_child("assets")) {
        spdlog::warn("Manifest has no 'assets' section");
        return;
    }

    ryml::ConstNodeRef assets = root["assets"];

    // Iterate categories (raw, textures, shaders, etc.)
    for (ryml::ConstNodeRef category : assets.children()) {
        std::string categoryName = toStdString(category.key());

        // Iterate assets in category
        for (ryml::ConstNodeRef assetNode : category.children()) {
            std::string assetName = toStdString(assetNode.key());

            AssetEntry entry;
            entry.category = categoryName;

            // Clone the asset's config subtree so it persists
            // We create a new tree that contains just this asset's config
            ryml::Tree subtree;
            subtree.reserve(assetNode.num_children() * 2 + 1);

            // Copy the node structure
            ryml::NodeRef subtreeRoot = subtree.rootref();
            subtreeRoot |= ryml::MAP;

            // Copy nodes to new tree, copying strings to the new tree's arena
            // (csubstr from the original tree would become dangling pointers)
            for (ryml::ConstNodeRef child : assetNode.children()) {
                // Copy key string to new tree's arena
                ryml::csubstr key = subtree.copy_to_arena(child.key());

                if (child.is_keyval()) {
                    // Simple key-value pair - copy value to arena too
                    ryml::csubstr val = subtree.copy_to_arena(child.val());
                    subtreeRoot[key] = val;
                } else if (child.has_children()) {
                    // Nested node
                    ryml::NodeRef newChild = subtreeRoot.append_child();
                    newChild.set_key(key);

                    if (child.is_map()) {
                        newChild |= ryml::MAP;
                    } else if (child.is_seq()) {
                        newChild |= ryml::SEQ;
                    }

                    for (ryml::ConstNodeRef grandchild : child.children()) {
                        if (grandchild.is_keyval()) {
                            ryml::NodeRef gc = newChild.append_child();
                            gc.set_key(subtree.copy_to_arena(grandchild.key()));
                            gc.set_val(subtree.copy_to_arena(grandchild.val()));
                        } else if (grandchild.has_val()) {
                            ryml::NodeRef gc = newChild.append_child();
                            gc.set_val(subtree.copy_to_arena(grandchild.val()));
                        }
                    }
                }
            }

            entry.configTree = std::move(subtree);
            entry.config = entry.configTree.rootref();

            if (entry.category == "shaders") {
                auto readNodeString = [](ryml::ConstNodeRef node, const char* key)
                    -> std::optional<std::string> {
                    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
                        return std::nullopt;
                    }
                    std::string value;
                    node[ryml::to_csubstr(key)] >> value;
                    return value;
                };

                auto computeOpt = readNodeString(assetNode, "compute");
                auto vertexOpt = readNodeString(assetNode, "vertex");
                auto fragmentOpt = readNodeString(assetNode, "fragment");

                ryml::NodeRef root = entry.configTree.rootref();
                if (vertexOpt) {
                    root[ryml::to_csubstr("vertex")] =
                        entry.configTree.copy_to_arena(ryml::to_csubstr(*vertexOpt));
                }
                if (fragmentOpt) {
                    root[ryml::to_csubstr("fragment")] =
                        entry.configTree.copy_to_arena(ryml::to_csubstr(*fragmentOpt));
                }

                if (!computeOpt && vertexOpt && (!fragmentOpt || fragmentOpt->empty())) {
                    std::string candidate = *vertexOpt;
                    size_t pos = candidate.rfind(".vert");
                    if (pos != std::string::npos) {
                        candidate.replace(pos, 5, ".frag");
                        root[ryml::to_csubstr("fragment")] =
                            entry.configTree.copy_to_arena(ryml::to_csubstr(candidate));
                    }
                }
            }

            // Build full asset ID: category/name
            std::string fullId = categoryName + "/" + assetName;

            // Log the path if present, or note that it's a complex asset
            auto pathOpt = entry.getString("path");
            if (pathOpt) {
                spdlog::debug("Registered asset: {} -> {}", fullId, *pathOpt);
            } else {
                spdlog::debug("Registered asset: {} (complex config)", fullId);
            }

            m_entries[fullId] = std::move(entry);

            // Fix the config reference after move - ConstNodeRef is just a pointer,
            // so it must be regenerated to point to the moved tree's new location
            m_entries[fullId].config = m_entries[fullId].configTree.rootref();
        }
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

const AssetManager::AssetEntry* AssetManager::getEntry(const std::string& id) const {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return nullptr;
    }
    return &it->second;
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

std::shared_ptr<RawAsset> AssetManager::loadRawAsset(
    const std::string& id,
    const AssetEntry& entry
) {
    auto pathOpt = entry.getString("path");
    if (!pathOpt) {
        throw AssetLoadError(id, "Raw asset missing 'path' field");
    }

    spdlog::debug("Loading raw asset: {} from {}", id, *pathOpt);

    auto data = ResourceRegistry::Get(*pathOpt);

    auto asset = std::make_shared<RawAsset>();
    asset->data.assign(data.begin(), data.end());

    return asset;
}

std::shared_ptr<TextureAsset> AssetManager::loadTextureAsset(
    const std::string& id,
    const AssetEntry& entry
) {
    auto pathOpt = entry.getString("path");
    if (!pathOpt) {
        throw AssetLoadError(id, "Texture asset missing 'path' field");
    }

    spdlog::debug("Loading texture asset: {} from {}", id, *pathOpt);

    // Get raw image data
    auto data = ResourceRegistry::Get(*pathOpt);

    // Load with stb_image
    int width, height, channels;
    stbi_set_flip_vertically_on_load(true);  // OpenGL expects bottom-left origin

    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()),
        &width, &height, &channels, 0
    );

    if (!pixels) {
        throw AssetLoadError(id, std::string("stb_image failed: ") + stbi_failure_reason());
    }

    // Create OpenGL texture
    auto asset = std::make_shared<TextureAsset>();
    asset->width = width;
    asset->height = height;
    asset->channels = channels;

    glGenTextures(1, &asset->id);
    glBindTexture(GL_TEXTURE_2D, asset->id);

    // Determine format
    GLenum format = GL_RGB;
    GLenum internalFormat = GL_RGB8;
    if (channels == 4) {
        format = GL_RGBA;
        internalFormat = GL_RGBA8;
    } else if (channels == 1) {
        format = GL_RED;
        internalFormat = GL_R8;
    }

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                 format, GL_UNSIGNED_BYTE, pixels);

    // Set filtering based on properties
    GLenum minFilter = GL_NEAREST;
    GLenum magFilter = GL_NEAREST;

    auto filterOpt = entry.getString("filter");
    if (filterOpt && *filterOpt == "linear") {
        minFilter = GL_LINEAR;
        magFilter = GL_LINEAR;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Free stb_image data
    stbi_image_free(pixels);

    spdlog::debug("Loaded texture: {}x{} ({} channels)", width, height, channels);

    return asset;
}

} // namespace Rigel::Asset
