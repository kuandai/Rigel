#include "TestFramework.h"

#include "Rigel/Config/ConfigSource.h"

using namespace Rigel;

TEST_CASE(StandardConfigSources_PreservePrecedenceAndProvenance) {
    Asset::AssetManager assets;

    auto sources = Config::makeStandardConfigSources(
        assets, "raw/example_config", "example.yaml", 42);

    CHECK_EQ(sources.size(), static_cast<size_t>(4));
    CHECK_EQ(sources[0]->name(), "raw/example_config");
    CHECK_EQ(sources[1]->name(), "config/example.yaml");
    CHECK_EQ(sources[2]->name(), "example.yaml");
    CHECK_EQ(sources[3]->name(), "config/worlds/42/example.yaml");
}
