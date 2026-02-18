#include "TestFramework.h"

#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

TEST_CASE(ImGuiLayer_RenderProfilerWindow_AcceptsOptionalVoxelSvoTelemetry) {
    Rigel::Voxel::VoxelSvoConfig voxelConfig;
    Rigel::Voxel::VoxelSvoTelemetry voxelTelemetry;

    CHECK_NO_THROW(Rigel::UI::renderProfilerWindow(false, &voxelConfig, &voxelTelemetry));
}
