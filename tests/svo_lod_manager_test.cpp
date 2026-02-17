#include "TestFramework.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/Lod/SvoLodManager.h"

#include <cmath>
#include <stdexcept>

using namespace Rigel::Voxel;

namespace {

BlockID registerStone(BlockRegistry& registry) {
    BlockType stone;
    stone.identifier = "rigel:stone";
    stone.isOpaque = true;
    registry.registerBlock(stone.identifier, stone);
    auto id = registry.findByIdentifier(stone.identifier);
    if (!id) {
        throw std::runtime_error("Failed to register stone block");
    }
    return *id;
}

BlockID registerWater(BlockRegistry& registry) {
    BlockType water;
    water.identifier = "rigel:water";
    water.isOpaque = false;
    registry.registerBlock(water.identifier, water);
    auto id = registry.findByIdentifier(water.identifier);
    if (!id) {
        throw std::runtime_error("Failed to register water block");
    }
    return *id;
}

void placeStone(ChunkManager& manager, BlockID stone, int wx, int wy, int wz) {
    BlockState state;
    state.id = stone;
    manager.setBlock(wx, wy, wz, state);
}

} // namespace

TEST_CASE(SvoLodManager_ConfigIsSanitized) {
    SvoLodManager manager;

    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = -3;
    config.lodStartRadiusChunks = -7;
    config.lodCellSpanChunks = 0;
    config.lodMaxCells = -11;
    config.lodCopyBudgetPerFrame = -1;
    config.lodApplyBudgetPerFrame = -2;

    manager.setConfig(config);
    const auto& effective = manager.config();

    CHECK(effective.enabled);
    CHECK_EQ(effective.nearMeshRadiusChunks, 0);
    CHECK_EQ(effective.lodStartRadiusChunks, 0);
    CHECK_EQ(effective.lodCellSpanChunks, 1);
    CHECK_EQ(effective.lodMaxCells, 0);
    CHECK_EQ(effective.lodCopyBudgetPerFrame, 0);
    CHECK_EQ(effective.lodApplyBudgetPerFrame, 0);
}

TEST_CASE(SvoLodManager_UpdateStaysInertWhenDisabled) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);
    placeStone(chunkManager, stone, 33, 33, 33);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.initialize();
    manager.update(glm::vec3(1.0f, 2.0f, 3.0f));

    const auto& telemetry = manager.telemetry();
    CHECK_EQ(telemetry.updateCalls, 0u);
    CHECK_EQ(telemetry.copiedCells, 0u);
    CHECK_EQ(telemetry.appliedCells, 0u);
    CHECK_EQ(telemetry.activeCells, 0u);
}

TEST_CASE(SvoLodManager_CopyBudgetLimitsPerFrame) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 33, 33, 33);
    placeStone(chunkManager, stone, 161, 33, 33);
    placeStone(chunkManager, stone, 289, 33, 33);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 1;
    config.lodApplyBudgetPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    CHECK_EQ(manager.telemetry().copiedCells, 1u);

    manager.update(glm::vec3(0.0f));
    CHECK_EQ(manager.telemetry().copiedCells, 2u);

    manager.update(glm::vec3(0.0f));
    CHECK_EQ(manager.telemetry().copiedCells, 3u);
}

TEST_CASE(SvoLodManager_ApplyBudgetLimitsPerFrame) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);
    placeStone(chunkManager, stone, 33, 33, 33);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 1;
    config.lodApplyBudgetPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    CHECK_EQ(manager.telemetry().copiedCells, 1u);
    CHECK_EQ(manager.telemetry().appliedCells, 0u);

    config.lodApplyBudgetPerFrame = 1;
    manager.setConfig(config);
    manager.update(glm::vec3(0.0f));
    CHECK_EQ(manager.telemetry().appliedCells, 1u);
}

TEST_CASE(SvoLodManager_StaleRevisionOutputsAreDropped) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);
    placeStone(chunkManager, stone, 33, 33, 33);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 1;
    config.lodApplyBudgetPerFrame = 0;
    manager.setConfig(config);
    manager.initialize();

    const LodCellKey key = chunkToLodCell({1, 0, 0}, config.lodCellSpanChunks);

    manager.update(glm::vec3(0.0f));
    auto info = manager.cellInfo(key);
    CHECK(info.has_value());
    CHECK_EQ(info->desiredRevision, 1u);
    CHECK_EQ(info->queuedRevision, 1u);
    CHECK_EQ(info->appliedRevision, 0u);

    placeStone(chunkManager, stone, 34, 33, 33);
    manager.update(glm::vec3(0.0f));
    info = manager.cellInfo(key);
    CHECK(info.has_value());
    CHECK(info->desiredRevision > 1u);
    CHECK(info->queuedRevision >= 1u);
    const uint64_t desiredRevisionAfterEdit = info->desiredRevision;

    config.lodApplyBudgetPerFrame = 1;
    manager.setConfig(config);
    manager.update(glm::vec3(0.0f));
    info = manager.cellInfo(key);
    CHECK(info.has_value());
    CHECK(info->appliedRevision < desiredRevisionAfterEdit);

    bool reachedReady = false;
    for (int i = 0; i < 8; ++i) {
        manager.update(glm::vec3(0.0f));
        info = manager.cellInfo(key);
        CHECK(info.has_value());
        if (info->state == LodCellState::Ready &&
            info->appliedRevision == desiredRevisionAfterEdit) {
            reachedReady = true;
            break;
        }
    }
    CHECK(reachedReady);
}

TEST_CASE(SvoLodManager_BuildsOccupancyMaterialHierarchyPerCell) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);
    BlockID water = registerWater(registry);

    placeStone(chunkManager, stone, 33, 33, 33);
    placeStone(chunkManager, water, 65, 33, 33);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 8;
    config.lodApplyBudgetPerFrame = 8;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    const LodCellKey key = chunkToLodCell({1, 0, 0}, config.lodCellSpanChunks);
    const auto info = manager.cellInfo(key);
    CHECK(info.has_value());
    CHECK_EQ(info->state, LodCellState::Ready);
    CHECK(info->nodeCount >= 3u);
    CHECK(info->leafCount >= 2u);
    CHECK(info->mixedNodeCount >= 1u);
    CHECK(manager.telemetry().pendingUploads >= 1u);
    CHECK_EQ(manager.telemetry().uploadedCells, 0u);
}

TEST_CASE(SvoLodManager_CollectOpaqueDrawInstances_ExcludesNonOpaqueLeaves) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);
    BlockID water = registerWater(registry);

    placeStone(chunkManager, stone, 33, 33, 33); // chunk (1,1,1), opaque
    placeStone(chunkManager, water, 65, 33, 33); // chunk (2,1,1), non-opaque

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 8;
    config.lodApplyBudgetPerFrame = 8;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    std::vector<SvoLodManager::OpaqueDrawInstance> instances;
    manager.collectOpaqueDrawInstances(instances);
    CHECK(!instances.empty());

    bool foundStoneChunk = false;
    bool foundWaterChunk = false;
    for (const auto& instance : instances) {
        CHECK(instance.worldSize > 0.0f);
        if (std::abs(instance.worldMin.x - 32.0f) < 0.01f) {
            foundStoneChunk = true;
        }
        if (std::abs(instance.worldMin.x - 64.0f) < 0.01f) {
            foundWaterChunk = true;
        }
    }

    CHECK(foundStoneChunk);
    CHECK(!foundWaterChunk);
}
