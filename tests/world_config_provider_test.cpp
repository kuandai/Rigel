#include "TestFramework.h"

#include "Rigel/Voxel/WorldConfigProvider.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

using namespace Rigel::Voxel;

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

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const std::filesystem::path& path)
        : m_original(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~CurrentPathGuard() {
        std::filesystem::current_path(m_original);
    }

private:
    std::filesystem::path m_original;
};

} // namespace

TEST_CASE(WorldConfigProvider_FileSource) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "rigel_world_config_test.yaml";
    {
        std::ofstream out(path);
        out << "seed: 99\n";
        out << "solid_block: base:stone_shale\n";
    }

    ConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(path.string()));
    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_EQ(config.seed, static_cast<uint32_t>(99));
    CHECK_EQ(config.solidBlock, "base:stone_shale");

    std::filesystem::remove(path);
}

TEST_CASE(WorldConfigProvider_OverlaySource) {
    std::filesystem::path basePath = std::filesystem::temp_directory_path() / "rigel_world_config_base.yaml";
    std::filesystem::path overlayPath = std::filesystem::temp_directory_path() / "rigel_world_config_overlay.yaml";
    {
        std::ofstream out(basePath);
        out << "flags:\n";
        out << "  smooth: true\n";
        out << "overlays:\n";
        out << "  - path: " << overlayPath.string() << "\n";
        out << "    when: smooth\n";
        out << "terrain:\n";
        out << "  base_height: 1.0\n";
    }
    {
        std::ofstream out(overlayPath);
        out << "terrain:\n";
        out << "  base_height: 9.0\n";
    }

    ConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(basePath.string()));
    WorldGenConfig config = provider.loadConfig().generation;

    CHECK_NEAR(config.terrain.baseHeight, 9.0f, 0.001f);

    std::filesystem::remove(basePath);
    std::filesystem::remove(overlayPath);
}

TEST_CASE(WorldConfigProvider_HigherPrecedenceSourceOverridesLowerOverlay) {
    ConfigProvider provider;
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

TEST_CASE(WorldConfigProvider_OverlayUsesDeclaringSource) {
    ConfigProvider provider;
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
    ConfigProvider provider;
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
    const auto root = std::filesystem::temp_directory_path()
        / "rigel_config_relative_overlay_test";
    const auto sourceDir = root / "source";
    const auto workingDir = root / "working";
    std::filesystem::remove_all(root);
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
    std::filesystem::remove_all(root);
}
