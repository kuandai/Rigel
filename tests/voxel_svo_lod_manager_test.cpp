#include "TestFramework.h"

#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

using namespace Rigel::Voxel;

TEST_CASE(VoxelSvoLodManager_ConfigIsSanitized) {
    VoxelSvoLodManager manager;
    VoxelSvoConfig config;
    config.enabled = true;
    config.nearMeshRadiusChunks = -1;
    config.startRadiusChunks = -2;
    config.maxRadiusChunks = -3;
    config.transitionBandChunks = -4;
    config.levels = 0;
    config.pageSizeVoxels = 9;
    config.minLeafVoxels = 7;
    config.buildBudgetPagesPerFrame = -1;
    config.applyBudgetPagesPerFrame = -2;
    config.uploadBudgetPagesPerFrame = -3;
    config.maxResidentPages = -4;
    config.maxCpuBytes = -5;
    config.maxGpuBytes = -6;

    manager.setConfig(config);

    const auto& effective = manager.config();
    CHECK(effective.enabled);
    CHECK_EQ(effective.nearMeshRadiusChunks, 0);
    CHECK_EQ(effective.startRadiusChunks, 0);
    CHECK_EQ(effective.maxRadiusChunks, 0);
    CHECK_EQ(effective.transitionBandChunks, 0);
    CHECK_EQ(effective.levels, 1);
    CHECK_EQ(effective.pageSizeVoxels, 16);
    CHECK_EQ(effective.minLeafVoxels, 8);
    CHECK_EQ(effective.buildBudgetPagesPerFrame, 0);
    CHECK_EQ(effective.applyBudgetPagesPerFrame, 0);
    CHECK_EQ(effective.uploadBudgetPagesPerFrame, 0);
    CHECK_EQ(effective.maxResidentPages, 0);
    CHECK_EQ(effective.maxCpuBytes, 0);
    CHECK_EQ(effective.maxGpuBytes, 0);
}

TEST_CASE(VoxelSvoLodManager_UpdateStaysInertWhenDisabled) {
    VoxelSvoLodManager manager;
    VoxelSvoConfig config;
    config.enabled = false;
    manager.setConfig(config);

    manager.initialize();
    manager.update(glm::vec3(0.0f));
    manager.uploadRenderResources();

    const auto& telemetry = manager.telemetry();
    CHECK_EQ(telemetry.updateCalls, 0u);
    CHECK_EQ(telemetry.uploadCalls, 0u);
    CHECK_EQ(telemetry.bricksSampled, 0u);
    CHECK_EQ(telemetry.voxelsSampled, 0u);
    CHECK_EQ(telemetry.loadedHits, 0u);
    CHECK_EQ(telemetry.persistenceHits, 0u);
    CHECK_EQ(telemetry.generatorHits, 0u);
    CHECK_EQ(telemetry.mipBuildMicros, 0u);
    CHECK_EQ(telemetry.activePages, 0u);
    CHECK_EQ(telemetry.pagesQueued, 0u);
    CHECK_EQ(telemetry.pagesBuilding, 0u);
    CHECK_EQ(telemetry.pagesReadyCpu, 0u);
    CHECK_EQ(telemetry.pagesUploaded, 0u);
    for (size_t i = 0; i < telemetry.readyCpuPagesPerLevel.size(); ++i) {
        CHECK_EQ(telemetry.readyCpuPagesPerLevel[i], 0u);
        CHECK_EQ(telemetry.readyCpuNodesPerLevel[i], 0u);
    }
}

TEST_CASE(VoxelSvoLodManager_ResetAndReinitialize_IsIdempotent) {
    VoxelSvoLodManager manager;
    VoxelSvoConfig config;
    config.enabled = true;
    manager.setConfig(config);

    CHECK_NO_THROW(manager.initialize());
    CHECK_NO_THROW(manager.update(glm::vec3(0.0f)));
    CHECK_NO_THROW(manager.uploadRenderResources());
    CHECK(manager.telemetry().updateCalls > 0u);

    CHECK_NO_THROW(manager.reset());
    CHECK_EQ(manager.telemetry().updateCalls, 0u);

    CHECK_NO_THROW(manager.initialize());
    CHECK_NO_THROW(manager.update(glm::vec3(1.0f)));
    CHECK(manager.telemetry().updateCalls > 0u);
}
