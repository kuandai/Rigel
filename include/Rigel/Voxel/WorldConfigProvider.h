#pragma once

#include "WorldGenConfig.h"

#include <Rigel/Asset/AssetManager.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rigel::Voxel {

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

class ConfigProvider {
public:
    void addSource(std::unique_ptr<IConfigSource> source);
    WorldGenConfig loadConfig() const;

private:
    std::vector<std::unique_ptr<IConfigSource>> m_sources;
};

} // namespace Rigel::Voxel
