#include "TestFramework.h"

#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/Voxel/Lod/SvoLodManager.h"

TEST_CASE(ImGuiLayer_RenderProfilerWindow_AcceptsOptionalSvoTelemetry) {
    Rigel::Voxel::SvoLodConfig config;
    Rigel::Voxel::SvoLodTelemetry telemetry;

    CHECK_NO_THROW(Rigel::UI::renderProfilerWindow(false, &config, &telemetry));
}
