#include "TestFramework.h"

#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Asset/Types.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

using namespace Rigel::Voxel;
using namespace Rigel::Config;

namespace {

class MemoryConfigSource : public IConfigSource {
public:
    MemoryConfigSource(std::string sourceName,
                       std::string yaml,
                       std::unordered_map<std::string, std::string> paths = {})
        : m_name(std::move(sourceName))
        , m_yaml(std::move(yaml))
        , m_paths(std::move(paths))
    {}

    std::optional<std::string> load() const override {
        return m_yaml;
    }

    std::string name() const override {
        return m_name;
    }

    std::optional<ConfigSourceResult> loadPath(std::string_view path) const override {
        auto it = m_paths.find(std::string(path));
        if (it == m_paths.end()) {
            return std::nullopt;
        }
        return ConfigSourceResult{m_name + ":" + it->first, it->second};
    }

private:
    std::string m_name;
    std::string m_yaml;
    std::unordered_map<std::string, std::string> m_paths;
};

class FixedRawLoader final : public Rigel::Asset::IAssetLoader {
public:
    explicit FixedRawLoader(std::string yaml)
        : m_yaml(std::move(yaml)) {
    }

    std::string_view category() const override {
        return "raw";
    }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext&) override {
        auto asset = std::make_shared<Rigel::Asset::RawAsset>();
        asset->data.assign(m_yaml.begin(), m_yaml.end());
        return asset;
    }

private:
    std::string m_yaml;
};

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const std::filesystem::path& path)
        : m_original(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~CurrentPathGuard() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
    }

private:
    std::filesystem::path m_original;
};

} // namespace

TEST_CASE(WorldConfigProvider_FileSource) {
    Rigel::Test::TemporaryDirectory directory("rigel_world_config");
    const auto path = directory.path() / "world.yaml";
    {
        std::ofstream out(path);
        out << "seed: 99\n";
        out << "solid_block: base:stone_shale\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(path.string()));
    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_EQ(config.seed, static_cast<uint32_t>(99));
    CHECK_EQ(config.solidBlock, "base:stone_shale");
}

TEST_CASE(WorldConfigProvider_OverlaySource) {
    Rigel::Test::TemporaryDirectory directory("rigel_world_config");
    const auto basePath = directory.path() / "base.yaml";
    const auto overlayPath = directory.path() / "overlay.yaml";
    {
        std::ofstream out(basePath);
        out << "flags:\n";
        out << "  smooth: true\n";
        out << "overlays:\n";
        out << "  - path: overlay.yaml\n";
        out << "    when: smooth\n";
        out << "terrain:\n";
        out << "  base_height: 1.0\n";
    }
    {
        std::ofstream out(overlayPath);
        out << "terrain:\n";
        out << "  base_height: 9.0\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(basePath.string()));
    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 9.0f, 0.001f);
}

TEST_CASE(WorldConfigProvider_MissingRelativeOverlayIsFatal) {
    Rigel::Test::TemporaryDirectory directory("rigel_missing_relative_overlay");
    const auto basePath = directory.path() / "base.yaml";
    {
        std::ofstream out(basePath);
        out << "terrain:\n";
        out << "  base_height: 9.0\n";
        out << "overlays:\n";
        out << "  - path: missing.yaml\n";
    }

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(basePath.string()));

    std::string diagnostic;
    try {
        (void)provider.loadConfig();
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }
    CHECK_EQ(
        diagnostic,
        "Missing configuration overlay '" +
            (directory.path() / "missing.yaml").string() +
            "' declared by '" + basePath.string() + "'");
}

TEST_CASE(WorldConfigProvider_MissingEmbeddedOverlayIsFatal) {
    Rigel::Asset::AssetManager assets;
    assets.registerLoader(
        "raw",
        std::make_unique<FixedRawLoader>(
            "overlays:\n"
            "  - path: assets/config/definitely_missing_overlay.yaml\n"));
    assets.loadManifest("manifest.yaml");

    WorldConfigProvider provider;
    provider.addSource(std::make_unique<EmbeddedConfigSource>(
        assets, "raw/world_config"));

    std::string diagnostic;
    try {
        (void)provider.loadConfig();
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }
    CHECK_EQ(
        diagnostic,
        "Missing configuration overlay 'config/definitely_missing_overlay.yaml' "
        "declared by 'raw/world_config'");
}

TEST_CASE(WorldConfigProvider_HigherPrecedenceSourceOverridesLowerOverlay) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "defaults",
        "terrain:\n"
        "  base_height: 1.0\n"
        "streaming:\n"
        "  worker_threads: 1\n"
        "overlays:\n"
        "  - path: tuning.yaml\n",
        std::unordered_map<std::string, std::string>{
            {
                "tuning.yaml",
                "terrain:\n"
                "  base_height: 2.0\n"
                "streaming:\n"
                "  worker_threads: 2\n"
            }
        }
    ));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "world",
        "terrain:\n"
        "  base_height: 9.0\n"
        "streaming:\n"
        "  worker_threads: 3\n"
    ));

    const WorldConfiguration config = provider.loadConfig();

    CHECK_NEAR(config.generation.terrain.baseHeight, 9.0f, 0.001f);
    CHECK_EQ(config.streaming.workerThreads, 3);
}

TEST_CASE(WorldConfigProvider_ValidatesCrossFieldsAfterAllSourcesMerge) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "base",
        "world:\n"
        "  min_y: 400\n"
        "streaming:\n"
        "  worker_threads: 64\n"
    ));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "override",
        "world:\n"
        "  max_y: 500\n"
        "streaming:\n"
        "  io_threads: 0\n"
        "  load_worker_threads: 0\n"
    ));

    const WorldConfiguration config = provider.loadConfig();
    CHECK_EQ(config.generation.world.minY, 400);
    CHECK_EQ(config.generation.world.maxY, 500);
    CHECK_EQ(config.streaming.workerThreads, 64);
    CHECK_EQ(config.streaming.ioThreads, 0);
    CHECK_EQ(config.streaming.loadWorkerThreads, 0);
}

TEST_CASE(WorldConfigProvider_ReportsInvalidFinalCrossFields) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "base",
        "streaming:\n"
        "  worker_threads: 61\n"
        "  io_threads: 2\n"
        "  load_worker_threads: 2\n"
    ));

    std::string message;
    try {
        (void)provider.loadConfig();
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    CHECK_EQ(
        message,
        "Invalid configuration value 'streaming.worker_threads' in "
        "'merged world configuration': combined worker_threads, io_threads, "
        "and load_worker_threads must not exceed 64"
    );
}

TEST_CASE(WorldConfigProvider_OverlayUsesDeclaringSource) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "defaults",
        "overlays:\n"
        "  - path: tuning.yaml\n",
        std::unordered_map<std::string, std::string>{
            {"tuning.yaml", "terrain:\n  base_height: 2.0\n"}
        }
    ));
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "world",
        "overlays:\n"
        "  - path: tuning.yaml\n",
        std::unordered_map<std::string, std::string>{
            {"tuning.yaml", "terrain:\n  base_height: 10.0\n"}
        }
    ));

    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 10.0f, 0.001f);
}

TEST_CASE(WorldConfigProvider_AppliesNestedOverlaysAfterDeclaredOverlays) {
    WorldConfigProvider provider;
    provider.addSource(std::make_unique<MemoryConfigSource>(
        "defaults",
        "terrain:\n"
        "  base_height: 1.0\n"
        "overlays:\n"
        "  - path: first.yaml\n"
        "  - path: second.yaml\n",
        std::unordered_map<std::string, std::string>{
            {
                "first.yaml",
                "terrain:\n"
                "  base_height: 2.0\n"
                "overlays:\n"
                "  - path: nested.yaml\n"
            },
            {"second.yaml", "terrain:\n  base_height: 3.0\n"},
            {"nested.yaml", "terrain:\n  base_height: 4.0\n"}
        }
    ));

    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 4.0f, 0.001f);
}

TEST_CASE(FileConfigSource_ResolvesOverlayRelativeToDeclaringFile) {
    Rigel::Test::TemporaryDirectory directory("rigel_config_relative_overlay");
    const auto sourceDir = directory.path() / "source";
    const auto workingDir = directory.path() / "working";
    std::filesystem::create_directories(sourceDir);
    std::filesystem::create_directories(workingDir);
    {
        std::ofstream out(sourceDir / "overlay.yaml");
        out << "terrain:\n  base_height: 9.0\n";
    }
    {
        std::ofstream out(workingDir / "overlay.yaml");
        out << "terrain:\n  base_height: 2.0\n";
    }

    std::optional<ConfigSourceResult> result;
    {
        CurrentPathGuard workingDirectory(workingDir);
        FileConfigSource source((sourceDir / "world_generation.yaml").string());
        result = source.loadPath("overlay.yaml");
    }

    CHECK(result.has_value());
    if (result) {
        CHECK(result->content.find("9.0") != std::string::npos);
        CHECK(result->content.find("2.0") == std::string::npos);
    }
}
