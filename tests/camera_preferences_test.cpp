#include "TestFramework.h"

#include "FrameRendererTestAccess.h"
#include "Rigel/Render/CameraProjection.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Render/TemporalJitter.h"

TEST_CASE(CameraPreferences_MainAndDebugProjectionUseOneVerticalFov) {
    const float fov = 93.0f;
    constexpr float mainAspect = 16.0f / 9.0f;
    Rigel::Render::FrameRenderer renderer;
    renderer.setVerticalFovDegrees(fov);

    const auto [mainProjection, debugProjection] =
        Rigel::Render::FrameRendererTestAccess::cameraProjections(
            renderer, mainAspect, 0.1f, 500.0f);

    CHECK_EQ(
        mainProjection,
        Rigel::Render::makeCameraProjection(
            fov, mainAspect, 0.1f, 500.0f));
    CHECK_EQ(
        debugProjection,
        Rigel::Render::makeCameraProjection(fov, 1.0f, 0.1f, 500.0f));
    CHECK_NE(
        mainProjection,
        Rigel::Render::makeCameraProjection(
            60.0f, mainAspect, 0.1f, 500.0f));
    CHECK_NE(
        debugProjection,
        Rigel::Render::makeCameraProjection(60.0f, 1.0f, 0.1f, 500.0f));
}

TEST_CASE(CameraPreferences_FovChangeInvalidatesHistoryAndRestartsJitter) {
    Rigel::Render::FrameRenderer renderer;
    renderer.setVerticalFovDegrees(60.0);
    static_cast<void>(
        Rigel::Render::FrameRendererTestAccess::nextTemporalJitter(
            renderer, 800, 600, 1.0f));
    Rigel::Render::FrameRendererTestAccess::markTemporalHistoryValid(renderer);

    renderer.setVerticalFovDegrees(87.0);

    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::verticalFovDegrees(renderer),
        87.0);
    CHECK(!Rigel::Render::FrameRendererTestAccess::temporalHistoryValid(
        renderer));

    Rigel::Render::TemporalJitterSequence expected;
    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::nextTemporalJitter(
            renderer, 800, 600, 1.0f),
        expected.next(800, 600, 1.0f));

    renderer.setVerticalFovDegrees(87.0);
    CHECK_EQ(
        Rigel::Render::FrameRendererTestAccess::nextTemporalJitter(
            renderer, 800, 600, 1.0f),
        expected.next(800, 600, 1.0f));
}
