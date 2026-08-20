#pragma once

#include <Rigel/Asset/AssetManager.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rigel::Config {

struct ConfigSourceResult {
    std::string name;
    std::string content;
};

class IConfigSource {
public:
    virtual ~IConfigSource() = default;
    virtual std::optional<std::string> load() const = 0;
    virtual std::string name() const = 0;
    virtual std::optional<ConfigSourceResult> loadPath(std::string_view path) const;
};

class EmbeddedConfigSource : public IConfigSource {
public:
    EmbeddedConfigSource(Asset::AssetManager& assets, std::string assetId);

    std::optional<std::string> load() const override;
    std::string name() const override;
    std::optional<ConfigSourceResult> loadPath(std::string_view path) const override;

private:
    Asset::AssetManager& m_assets;
    std::string m_assetId;
};

class FileConfigSource : public IConfigSource {
public:
    explicit FileConfigSource(std::string path);

    std::optional<std::string> load() const override;
    std::string name() const override;
    std::optional<ConfigSourceResult> loadPath(std::string_view path) const override;

private:
    std::string m_path;
};

std::vector<std::unique_ptr<IConfigSource>> makeStandardConfigSources(
    Asset::AssetManager& assets,
    std::string embeddedAssetId,
    std::string fileName,
    std::uint32_t worldId);

} // namespace Rigel::Config
