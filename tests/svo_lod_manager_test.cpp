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
    config.lodMaxCpuBytes = -12;
    config.lodMaxGpuBytes = -13;
    config.lodCopyBudgetPerFrame = -1;
    config.lodApplyBudgetPerFrame = -2;

    manager.setConfig(config);
    const auto& effective = manager.config();

    CHECK(effective.enabled);
    CHECK_EQ(effective.nearMeshRadiusChunks, 0);
    CHECK_EQ(effective.lodStartRadiusChunks, 0);
    CHECK_EQ(effective.lodCellSpanChunks, 1);
    CHECK_EQ(effective.lodMaxCells, 0);
    CHECK_EQ(effective.lodMaxCpuBytes, 0);
    CHECK_EQ(effective.lodMaxGpuBytes, 0);
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
    CHECK_EQ(telemetry.scanMicros, 0u);
    CHECK_EQ(telemetry.copyMicros, 0u);
    CHECK_EQ(telemetry.applyMicros, 0u);
    CHECK_EQ(telemetry.uploadMicros, 0u);
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
    CHECK(manager.telemetry().cellsReady >= 1u);
    CHECK(manager.telemetry().cpuBytesCurrent > 0u);
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
    manager.collectOpaqueDrawInstances(instances, glm::vec3(-400.0f, 0.0f, 0.0f), 1024.0f);
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

TEST_CASE(SvoLodManager_CollectOpaqueDrawInstances_RespectsLodDistanceBands) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 33, 33, 33);   // near cell
    placeStone(chunkManager, stone, 257, 33, 33);  // far cell

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 1;
    config.lodStartRadiusChunks = 2;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    std::vector<SvoLodManager::OpaqueDrawInstance> instances;
    manager.collectOpaqueDrawInstances(instances, glm::vec3(0.0f), 1024.0f);
    CHECK(!instances.empty());

    bool foundNear = false;
    bool foundFar = false;
    for (const auto& instance : instances) {
        if (std::abs(instance.worldMin.x - 0.0f) < 0.01f) {
            foundNear = true;
        }
        if (std::abs(instance.worldMin.x - 256.0f) < 0.01f) {
            foundFar = true;
        }
    }

    CHECK(!foundNear);
    CHECK(foundFar);

    manager.collectOpaqueDrawInstances(instances, glm::vec3(0.0f), 64.0f);
    CHECK(instances.empty());
}

TEST_CASE(SvoLodManager_CollectOpaqueDrawInstances_UsesHysteresis) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 257, 33, 33); // cell at x=2 for span=4

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 2;
    config.lodStartRadiusChunks = 6;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    std::vector<SvoLodManager::OpaqueDrawInstance> instances;
    manager.collectOpaqueDrawInstances(instances, glm::vec3(0.0f), 1024.0f);
    CHECK(!instances.empty());

    manager.collectOpaqueDrawInstances(instances, glm::vec3(120.0f, 0.0f, 0.0f), 1024.0f);
    CHECK(!instances.empty());

    manager.collectOpaqueDrawInstances(instances, glm::vec3(220.0f, 0.0f, 0.0f), 1024.0f);
    CHECK(instances.empty());
}

TEST_CASE(SvoLodManager_CollectOpaqueDrawInstances_ReturnsNoneWhenDisabled) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 289, 33, 33);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 1;
    config.lodStartRadiusChunks = 2;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    std::vector<SvoLodManager::OpaqueDrawInstance> instances;
    manager.collectOpaqueDrawInstances(instances, glm::vec3(0.0f), 1024.0f);
    CHECK(!instances.empty());

    config.enabled = false;
    manager.setConfig(config);
    manager.collectOpaqueDrawInstances(instances, glm::vec3(0.0f), 1024.0f);
    CHECK(instances.empty());
}

TEST_CASE(SvoLodManager_EvictsFarthestCellsFirstWhenCellBudgetExceeded) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 33, 33, 33);    // chunk x=1 -> cell x=0
    placeStone(chunkManager, stone, 289, 33, 33);   // chunk x=9 -> cell x=2
    placeStone(chunkManager, stone, 545, 33, 33);   // chunk x=17 -> cell x=4

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodMaxCells = 3;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    const LodCellKey cellNear = chunkToLodCell({1, 0, 0}, config.lodCellSpanChunks);
    const LodCellKey cellMid = chunkToLodCell({9, 0, 0}, config.lodCellSpanChunks);
    const LodCellKey cellFar = chunkToLodCell({17, 0, 0}, config.lodCellSpanChunks);

    CHECK(manager.cellInfo(cellNear).has_value());
    CHECK(manager.cellInfo(cellMid).has_value());
    CHECK(manager.cellInfo(cellFar).has_value());

    config.lodMaxCells = 2;
    manager.setConfig(config);
    manager.update(glm::vec3(0.0f));

    CHECK(manager.cellInfo(cellNear).has_value());
    CHECK(manager.cellInfo(cellMid).has_value());
    CHECK(!manager.cellInfo(cellFar).has_value());
}

TEST_CASE(SvoLodManager_EvictsByCpuByteBudgetUsingDistanceLruPolicy) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 33, 33, 33);    // chunk x=1 -> cell x=0
    placeStone(chunkManager, stone, 289, 33, 33);   // chunk x=9 -> cell x=2
    placeStone(chunkManager, stone, 545, 33, 33);   // chunk x=17 -> cell x=4

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    const LodCellKey cellNear = chunkToLodCell({1, 0, 0}, config.lodCellSpanChunks);
    const LodCellKey cellMid = chunkToLodCell({9, 0, 0}, config.lodCellSpanChunks);
    const LodCellKey cellFar = chunkToLodCell({17, 0, 0}, config.lodCellSpanChunks);

    const auto nearInfo = manager.cellInfo(cellNear);
    const auto midInfo = manager.cellInfo(cellMid);
    const auto farInfo = manager.cellInfo(cellFar);
    CHECK(nearInfo.has_value());
    CHECK(midInfo.has_value());
    CHECK(farInfo.has_value());

    const int64_t nearBytes =
        static_cast<int64_t>(nearInfo->nodeCount) * static_cast<int64_t>(sizeof(LodSvoNode));
    const int64_t midBytes =
        static_cast<int64_t>(midInfo->nodeCount) * static_cast<int64_t>(sizeof(LodSvoNode));
    const int64_t farBytes =
        static_cast<int64_t>(farInfo->nodeCount) * static_cast<int64_t>(sizeof(LodSvoNode));
    const int64_t totalBytes = nearBytes + midBytes + farBytes;
    CHECK(totalBytes > 0);
    CHECK(farBytes > 0);

    config.lodMaxCells = 0;
    config.lodMaxCpuBytes = totalBytes - farBytes;
    manager.setConfig(config);
    manager.update(glm::vec3(0.0f));

    CHECK(manager.cellInfo(cellNear).has_value());
    CHECK(manager.cellInfo(cellMid).has_value());
    CHECK(!manager.cellInfo(cellFar).has_value());
}

TEST_CASE(SvoLodManager_ToggleEnabled_DoesNotMutateChunkDataOrPersistFlag) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 33, 33, 33);
    ChunkCoord coord = worldToChunk(33, 33, 33);
    Chunk* chunk = chunkManager.getChunk(coord);
    CHECK(chunk != nullptr);
    if (!chunk) {
        return;
    }
    chunk->clearPersistDirty();

    const BlockID beforeId = chunkManager.getBlock(33, 33, 33).id;
    CHECK_EQ(beforeId, stone);

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));
    CHECK_EQ(chunkManager.getBlock(33, 33, 33).id, beforeId);
    CHECK(!chunk->isPersistDirty());

    config.enabled = false;
    manager.setConfig(config);
    manager.update(glm::vec3(0.0f));
    CHECK_EQ(chunkManager.getBlock(33, 33, 33).id, beforeId);
    CHECK(!chunk->isPersistDirty());

    BlockState air;
    chunkManager.setBlock(33, 33, 33, air);
    CHECK_EQ(chunkManager.getBlock(33, 33, 33).id, BlockRegistry::airId());
    CHECK(chunk->isPersistDirty());

    chunk->clearPersistDirty();
    config.enabled = true;
    manager.setConfig(config);
    manager.update(glm::vec3(0.0f));
    CHECK_EQ(chunkManager.getBlock(33, 33, 33).id, BlockRegistry::airId());
    CHECK(!chunk->isPersistDirty());
}

TEST_CASE(SvoLodManager_CollectDebugCells_ReportsStateSpanAndVisibility) {
    BlockRegistry registry;
    ChunkManager chunkManager;
    chunkManager.setRegistry(&registry);
    BlockID stone = registerStone(registry);

    placeStone(chunkManager, stone, 289, 33, 33); // chunk x=9 -> cell x=2 for span=4

    SvoLodManager manager;
    manager.bind(&chunkManager, &registry);
    manager.setBuildThreads(0);

    SvoLodConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = 1;
    config.lodStartRadiusChunks = 2;
    config.lodCellSpanChunks = 4;
    config.lodCopyBudgetPerFrame = 16;
    config.lodApplyBudgetPerFrame = 16;
    manager.setConfig(config);
    manager.initialize();

    manager.update(glm::vec3(0.0f));
    manager.update(glm::vec3(0.0f));

    const LodCellKey expectedKey = chunkToLodCell({9, 0, 0}, config.lodCellSpanChunks);

    std::vector<SvoLodManager::DebugCellState> debugCells;
    manager.collectDebugCells(debugCells);
    CHECK(!debugCells.empty());

    bool found = false;
    for (const auto& cell : debugCells) {
        if (cell.key != expectedKey) {
            continue;
        }
        found = true;
        CHECK_EQ(cell.state, LodCellState::Ready);
        CHECK_EQ(cell.spanChunks, 4);
        CHECK(!cell.visibleAsFarLod);
    }
    CHECK(found);

    std::vector<SvoLodManager::OpaqueDrawInstance> instances;
    manager.collectOpaqueDrawInstances(instances, glm::vec3(0.0f), 1024.0f);
    CHECK(!instances.empty());

    manager.collectDebugCells(debugCells);
    bool visibleFound = false;
    for (const auto& cell : debugCells) {
        if (cell.key != expectedKey) {
            continue;
        }
        visibleFound = true;
        CHECK(cell.visibleAsFarLod);
    }
    CHECK(visibleFound);

    config.enabled = false;
    manager.setConfig(config);
    manager.collectDebugCells(debugCells);
    CHECK(debugCells.empty());
}
