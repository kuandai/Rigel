#include "TestFramework.h"

#include "Rigel/Voxel/WorldConfigProvider.h"

#include <filesystem>
#include <fstream>

using namespace Rigel::Voxel;

TEST_CASE(WorldConfigProvider_FileSource) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "rigel_world_config_test.yaml";
    {
        std::ofstream out(path);
        out << "seed: 99\n";
        out << "solid_block: rigel:stone\n";
    }

    ConfigProvider provider;
    provider.addSource(std::make_unique<FileConfigSource>(path.string()));
    WorldGenConfig config = provider.loadConfig();

    CHECK_EQ(config.seed, static_cast<uint32_t>(99));
    CHECK_EQ(config.solidBlock, "rigel:stone");

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
    WorldGenConfig config = provider.loadConfig();

    CHECK_NEAR(config.terrain.baseHeight, 9.0f, 0.001f);

    std::filesystem::remove(basePath);
    std::filesystem::remove(overlayPath);
}
