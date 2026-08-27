#include "Rigel/Config/ConfigSource.h"

#include "Rigel/Asset/Types.h"

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

} // namespace

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

FileConfigSource::FileConfigSource(std::string path)
    : m_path(std::move(path))
{}

std::optional<std::string> FileConfigSource::load() const {
    return readFile(m_path);
}

std::string FileConfigSource::name() const {
    return m_path;
}

std::vector<std::unique_ptr<IConfigSource>> makeStandardConfigSources(
    Asset::AssetManager& assets,
    std::string embeddedAssetId,
    std::string fileName,
    std::uint32_t worldId) {
    std::vector<std::unique_ptr<IConfigSource>> sources;
    sources.reserve(4);
    sources.push_back(std::make_unique<EmbeddedConfigSource>(
        assets, std::move(embeddedAssetId)));
    sources.push_back(std::make_unique<FileConfigSource>(
        "config/" + fileName));
    sources.push_back(std::make_unique<FileConfigSource>(fileName));
    sources.push_back(std::make_unique<FileConfigSource>(
        "config/worlds/" + std::to_string(worldId) + "/" + fileName));
    return sources;
}

} // namespace Rigel::Config
