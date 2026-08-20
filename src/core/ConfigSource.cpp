#include "Rigel/Config/ConfigSource.h"

#include "Rigel/Asset/Types.h"
#include "ResourceRegistry.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace Rigel::Config {
namespace {

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string stripAssetsPrefix(std::string_view path) {
    constexpr std::string_view kAssetsPrefix = "assets/";
    if (path.substr(0, kAssetsPrefix.size()) == kAssetsPrefix) {
        return std::string(path.substr(kAssetsPrefix.size()));
    }
    return std::string(path);
}

} // namespace

std::optional<ConfigSourceResult> IConfigSource::loadPath(std::string_view) const {
    return std::nullopt;
}

EmbeddedConfigSource::EmbeddedConfigSource(Asset::AssetManager& assets,
                                           std::string assetId)
    : m_assets(assets)
    , m_assetId(std::move(assetId))
{}

std::optional<std::string> EmbeddedConfigSource::load() const {
    if (!m_assets.exists(m_assetId)) {
        return std::nullopt;
    }
    auto asset = m_assets.get<Asset::RawAsset>(m_assetId);
    return std::string(asset->data.begin(), asset->data.end());
}

std::string EmbeddedConfigSource::name() const {
    return m_assetId;
}

std::optional<ConfigSourceResult> EmbeddedConfigSource::loadPath(
    std::string_view path) const {
    std::string normalized = stripAssetsPrefix(path);
    try {
        auto data = ResourceRegistry::Get(normalized);
        return ConfigSourceResult{
            std::move(normalized),
            std::string(data.begin(), data.end())
        };
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

FileConfigSource::FileConfigSource(std::string path)
    : m_path(std::move(path))
{}

std::optional<std::string> FileConfigSource::load() const {
    return readFile(m_path);
}

std::string FileConfigSource::name() const {
    return m_path;
}

std::optional<ConfigSourceResult> FileConfigSource::loadPath(
    std::string_view path) const {
    std::filesystem::path candidate(path);
    if (!candidate.is_absolute()) {
        std::filesystem::path baseDir = std::filesystem::path(m_path).parent_path();
        if (!baseDir.empty()) {
            candidate = baseDir / candidate;
        }
    }
    auto content = readFile(candidate);
    if (!content) {
        return std::nullopt;
    }
    return ConfigSourceResult{candidate.string(), std::move(*content)};
}

} // namespace Rigel::Config
