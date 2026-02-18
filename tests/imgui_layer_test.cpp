#include "TestFramework.h"

#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/Voxel/Lod/SvoLodManager.h"
#include "Rigel/Voxel/VoxelLod/VoxelSvoLodManager.h"

TEST_CASE(ImGuiLayer_RenderProfilerWindow_AcceptsOptionalSvoTelemetry) {
    Rigel::Voxel::SvoLodConfig config;
    Rigel::Voxel::SvoLodTelemetry telemetry;
    Rigel::Voxel::VoxelSvoConfig voxelConfig;
    Rigel::Voxel::VoxelSvoTelemetry voxelTelemetry;

    CHECK_NO_THROW(Rigel::UI::renderProfilerWindow(false, &config, &telemetry,
                                                   &voxelConfig, &voxelTelemetry));
}
