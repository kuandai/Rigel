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
