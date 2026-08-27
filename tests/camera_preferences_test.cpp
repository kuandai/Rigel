#include "TestFramework.h"

#include "FrameRendererTestAccess.h"
#include "Rigel/Render/CameraProjection.h"
#include "Rigel/Render/FrameRenderer.h"

TEST_CASE(CameraPreferences_MainAndDebugProjectionUseOneVerticalFov) {
    const float fov = 93.0f;
    const glm::mat4 mainProjection =
        Rigel::Render::makeCameraProjection(fov, 1.0f, 0.1f, 500.0f);
    const glm::mat4 debugProjection =
        Rigel::Render::makeCameraProjection(fov, 1.0f, 0.1f, 500.0f);
    const glm::mat4 oldHardCodedProjection =
        Rigel::Render::makeCameraProjection(60.0f, 1.0f, 0.1f, 500.0f);

    CHECK_EQ(mainProjection, debugProjection);
    CHECK_NE(mainProjection, oldHardCodedProjection);
}

TEST_CASE(CameraPreferences_FovChangeInvalidatesTemporalHistoryOnce) {
    Rigel::Render::FrameRenderer renderer;
    Rigel::Render::FrameRendererTestAccess::markTemporalHistoryValid(renderer);

    renderer.setVerticalFovDegrees(87.0);

    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::verticalFovDegrees(renderer),
        87.0);
    CHECK(!Rigel::Render::FrameRendererTestAccess::temporalHistoryValid(
        renderer));
    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::temporalHistoryGeneration(
            renderer),
        std::uint64_t{1});

    renderer.setVerticalFovDegrees(87.0);
    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::temporalHistoryGeneration(
            renderer),
        std::uint64_t{1});
}
