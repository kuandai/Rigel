#include "TestFramework.h"

#include "Rigel/Core/DebugBlockCatalog.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"

#include <algorithm>
#include <random>
#include <string>

using namespace Rigel;

namespace {

Voxel::BlockRegistry makeRegistryWithBlocks(int count) {
    Voxel::BlockRegistry registry;
    for (int i = 0; i < count; ++i) {
        Voxel::BlockType type;
        const std::string id = "base:test_" + std::to_string(i);
        type.identifier = id;
        type.model = "cube";
        type.isOpaque = true;
        type.isSolid = true;
        type.textures = Voxel::FaceTextures::uniform("textures/blocks/test.png");
        registry.registerBlock(id, std::move(type));
    }
    return registry;
}

} // namespace

TEST_CASE(DebugBlockCatalog_EnvParsing) {
    CHECK(!Core::isDebugBlockCatalogEnabled(nullptr));
    CHECK(!Core::isDebugBlockCatalogEnabled(""));
    CHECK(!Core::isDebugBlockCatalogEnabled("0"));
    CHECK(Core::isDebugBlockCatalogEnabled("1"));
    CHECK(Core::isDebugBlockCatalogEnabled("true"));
}

TEST_CASE(DebugBlockCatalog_GatingPredicates) {
    CHECK(Core::shouldLoadWorldFromDisk(false));
    CHECK(!Core::shouldLoadWorldFromDisk(true));
    CHECK(Core::shouldCreateChunkLoader(false));
    CHECK(!Core::shouldCreateChunkLoader(true));
    CHECK(Core::shouldWireVoxelPersistenceSource(false));
    CHECK(!Core::shouldWireVoxelPersistenceSource(true));
    CHECK(Core::shouldSaveWorldToDisk(false));
    CHECK(!Core::shouldSaveWorldToDisk(true));
    CHECK(Core::shouldHandleBlockEdits(false));
    CHECK(!Core::shouldHandleBlockEdits(true));
    CHECK(Core::shouldHandleDemoSpawn(false));
    CHECK(!Core::shouldHandleDemoSpawn(true));
    CHECK(Core::shouldRunWorldStreaming(false));
    CHECK(!Core::shouldRunWorldStreaming(true));
}

TEST_CASE(DebugBlockCatalog_CollectBlockIds_ExcludesAir) {
    Voxel::BlockRegistry registry = makeRegistryWithBlocks(4);
    auto ids = Core::collectDebugBlockCatalogBlockIds(registry);
    CHECK_EQ(ids.size(), static_cast<size_t>(4));
    CHECK_EQ(ids.front().type, static_cast<uint16_t>(1));
    CHECK_EQ(ids.back().type, static_cast<uint16_t>(4));
}

TEST_CASE(DebugBlockCatalog_Placements_DeterministicSpacing) {
    Voxel::BlockRegistry registry = makeRegistryWithBlocks(5);
    Core::DebugBlockCatalogOptions options;
    options.columns = 2;
    options.spacing = 2;
    options.baseY = 64;
    options.originX = 10;
    options.originZ = 20;

    auto placements = Core::makeDebugBlockCatalogPlacements(registry, options);
    CHECK_EQ(placements.size(), static_cast<size_t>(5));

    CHECK_EQ(placements[0].blockId.type, static_cast<uint16_t>(1));
    CHECK_EQ(placements[0].worldX, 10);
    CHECK_EQ(placements[0].worldY, 64);
    CHECK_EQ(placements[0].worldZ, 20);

    CHECK_EQ(placements[1].blockId.type, static_cast<uint16_t>(2));
    CHECK_EQ(placements[1].worldX, 12);
    CHECK_EQ(placements[1].worldZ, 20);

    CHECK_EQ(placements[2].blockId.type, static_cast<uint16_t>(3));
    CHECK_EQ(placements[2].worldX, 10);
    CHECK_EQ(placements[2].worldZ, 22);

    CHECK_EQ(placements[4].blockId.type, static_cast<uint16_t>(5));
    CHECK_EQ(placements[4].worldX, 10);
    CHECK_EQ(placements[4].worldZ, 24);
}

TEST_CASE(DebugBlockCatalog_Layout_ComputesRowsAndCenter) {
    Core::DebugBlockCatalogOptions options;
    options.columns = 4;
    options.spacing = 2;
    options.baseY = 80;
    options.originX = 0;
    options.originZ = 4;

    auto layout = Core::makeDebugBlockCatalogLayout(10, options);
    CHECK_EQ(layout.blockCount, 10);
    CHECK_EQ(layout.columns, 4);
    CHECK_EQ(layout.rows, 3);
    CHECK_EQ(layout.baseY, 80);
    CHECK_NEAR(layout.centerX, 3.0f, 0.0001f);
    CHECK_NEAR(layout.centerZ, 6.0f, 0.0001f);
}

TEST_CASE(DebugBlockCatalog_RemeshChunks_ExpandsAtChunkBorders) {
    std::vector<Core::DebugBlockCatalogPlacement> placements;
    placements.push_back(Core::DebugBlockCatalogPlacement{
        .blockId = Voxel::BlockID{1},
        .worldX = 0,
        .worldY = 64,
        .worldZ = 0
    });
    placements.push_back(Core::DebugBlockCatalogPlacement{
        .blockId = Voxel::BlockID{2},
        .worldX = 1,
        .worldY = 64,
        .worldZ = 1
    });

    auto chunks = Core::collectDebugBlockCatalogRemeshChunks(placements);
    CHECK(!chunks.empty());

    auto hasChunk = [&](int x, int y, int z) {
        return std::find(chunks.begin(), chunks.end(), Voxel::ChunkCoord{x, y, z}) != chunks.end();
    };

    CHECK(hasChunk(0, 2, 0));
    CHECK(hasChunk(-1, 2, 0));
    CHECK(hasChunk(0, 1, 0));
    CHECK(hasChunk(0, 2, -1));
}

TEST_CASE(DebugBlockCatalog_Plan_BuildsCameraAndChunks) {
    Voxel::BlockRegistry registry = makeRegistryWithBlocks(20);
    Core::DebugBlockCatalogOptions options;
    options.columns = 6;
    options.spacing = 2;
    options.baseY = 70;

    auto plan = Core::buildDebugBlockCatalogPlan(registry, options);
    CHECK_EQ(plan.layout.blockCount, 20);
    CHECK_EQ(plan.layout.columns, 6);
    CHECK_EQ(plan.layout.rows, 4);
    CHECK_EQ(plan.placements.size(), static_cast<size_t>(20));
    CHECK(!plan.remeshChunks.empty());
    CHECK(plan.cameraPosition.y > plan.cameraTarget.y);
}

TEST_CASE(DebugBlockCatalog_Placements_DeterministicUnderRandomChecks) {
    Voxel::BlockRegistry registry = makeRegistryWithBlocks(32);
    Core::DebugBlockCatalogOptions options;
    options.columns = 7;
    options.spacing = 2;
    options.baseY = 77;
    options.originX = -13;
    options.originZ = 9;

    auto first = Core::makeDebugBlockCatalogPlacements(registry, options);
    auto second = Core::makeDebugBlockCatalogPlacements(registry, options);
    CHECK_EQ(first.size(), second.size());

    std::mt19937 rng(1337);
    std::uniform_int_distribution<size_t> dist(0, first.size() - 1);
    for (int i = 0; i < 50; ++i) {
        const size_t index = dist(rng);
        CHECK_EQ(first[index].blockId.type, second[index].blockId.type);
        CHECK_EQ(first[index].worldX, second[index].worldX);
        CHECK_EQ(first[index].worldY, second[index].worldY);
        CHECK_EQ(first[index].worldZ, second[index].worldZ);
    }
}

TEST_CASE(DebugBlockCatalog_ApplyPlacements_WritesExpectedBlocks) {
    Voxel::WorldResources resources;
    Voxel::BlockRegistry& registry = resources.registry();
    for (int i = 0; i < 3; ++i) {
        Voxel::BlockType type;
        const std::string id = "base:apply_" + std::to_string(i);
        type.identifier = id;
        type.model = "cube";
        type.isOpaque = true;
        type.isSolid = true;
        type.textures = Voxel::FaceTextures::uniform("textures/blocks/test.png");
        registry.registerBlock(id, std::move(type));
    }

    Voxel::World world(resources);
    Core::DebugBlockCatalogOptions options;
    options.columns = 2;
    options.spacing = 2;
    options.baseY = 64;
    options.originX = 0;
    options.originZ = 0;
    auto placements = Core::makeDebugBlockCatalogPlacements(registry, options);
    Core::applyDebugBlockCatalogPlacements(world, placements);

    CHECK_EQ(world.getBlock(0, 64, 0).id.type, static_cast<uint16_t>(1));
    CHECK_EQ(world.getBlock(2, 64, 0).id.type, static_cast<uint16_t>(2));
    CHECK_EQ(world.getBlock(0, 64, 2).id.type, static_cast<uint16_t>(3));
}

TEST_CASE(DebugBlockCatalog_ApplyPlacements_DoesNotTouchUntargetedCells) {
    Voxel::WorldResources resources;
    Voxel::BlockRegistry& registry = resources.registry();
    Voxel::BlockType type;
    const std::string id = "base:apply_single";
    type.identifier = id;
    type.model = "cube";
    type.isOpaque = true;
    type.isSolid = true;
    type.textures = Voxel::FaceTextures::uniform("textures/blocks/test.png");
    registry.registerBlock(id, std::move(type));

    Voxel::World world(resources);
    std::vector<Core::DebugBlockCatalogPlacement> placements{
        Core::DebugBlockCatalogPlacement{
            .blockId = Voxel::BlockID{1},
            .worldX = 4,
            .worldY = 64,
            .worldZ = 8
        }
    };
    Core::applyDebugBlockCatalogPlacements(world, placements);

    CHECK_EQ(world.getBlock(4, 64, 8).id.type, static_cast<uint16_t>(1));
    CHECK(world.getBlock(4, 64, 10).isAir());
    CHECK(world.getBlock(0, 64, 0).isAir());
    CHECK(world.getBlock(4, 63, 8).isAir());
}
