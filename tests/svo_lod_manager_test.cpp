#include "TestFramework.h"

#include "Rigel/Voxel/Lod/SvoLodManager.h"

using namespace Rigel::Voxel;

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
    SvoLodManager manager;
    manager.initialize();
    manager.update(glm::vec3(1.0f, 2.0f, 3.0f));

    const auto& telemetry = manager.telemetry();
    CHECK_EQ(telemetry.updateCalls, 0u);
    CHECK_EQ(telemetry.activeCells, 0u);
    CHECK_EQ(telemetry.pendingCopies, 0u);
    CHECK_EQ(telemetry.pendingApplies, 0u);
    CHECK_EQ(telemetry.copiedCells, 0u);
    CHECK_EQ(telemetry.appliedCells, 0u);
}

TEST_CASE(SvoLodManager_UpdateCountsAndResetClearsTelemetry) {
    SvoLodManager manager;

    SvoLodConfig config;
    config.enabled = true;
    manager.setConfig(config);

    manager.initialize();
    manager.update(glm::vec3(0.0f, 0.0f, 0.0f));
    manager.update(glm::vec3(4.0f, 0.0f, 2.0f));

    CHECK_EQ(manager.telemetry().updateCalls, 2u);

    manager.reset();
    const auto& telemetry = manager.telemetry();
    CHECK_EQ(telemetry.updateCalls, 0u);
    CHECK_EQ(telemetry.activeCells, 0u);
    CHECK_EQ(telemetry.pendingCopies, 0u);
    CHECK_EQ(telemetry.pendingApplies, 0u);
    CHECK_EQ(telemetry.copiedCells, 0u);
    CHECK_EQ(telemetry.appliedCells, 0u);
}
