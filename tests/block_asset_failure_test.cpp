#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Voxel/WorldResources.h"

#include <string>

using namespace Rigel::Asset;
using namespace Rigel::Voxel;

TEST_CASE(WorldResources_RejectsMissingBlockTexture) {
    AssetManager assets;
    WorldResources resources;
    std::string diagnostic;

    try {
        resources.initialize(assets);
    } catch (const std::exception& e) {
        diagnostic = e.what();
    }

    CHECK(!diagnostic.empty());
    CHECK(!resources.initialized());
    CHECK(diagnostic.find("0 definitions loaded") != std::string::npos);
    CHECK(diagnostic.find("1 failed") != std::string::npos);
    CHECK(diagnostic.find("0 textures loaded") != std::string::npos);
    CHECK(diagnostic.find("scripts/rigel_assets.py stage") != std::string::npos);
    CHECK(diagnostic.find("blocks/required_block.yaml") != std::string::npos);
    CHECK(diagnostic.find("textures/blocks/required.png") != std::string::npos);
}
