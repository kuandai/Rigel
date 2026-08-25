#include "TestFramework.h"

#include "Rigel/input/GameplayInput.h"

using namespace Rigel::Input;

TEST_CASE(CursorLock_CapturedCenterIsWindowMidpoint) {
    double x = 0.0;
    double y = 0.0;
    capturedCursorCenter(800, 600, x, y);
    CHECK_EQ(x, 400.0);
    CHECK_EQ(y, 300.0);

    capturedCursorCenter(0, 0, x, y);
    CHECK_EQ(x, 0.0);
    CHECK_EQ(y, 0.0);
}

TEST_CASE(CursorLock_FirstSampleIsOriginAndLaterSamplesRotate) {
    WindowState window;
    CameraState camera;
    camera.yaw = 10.0f;
    camera.pitch = 5.0f;
    camera.mouseSensitivity = 0.5f;

    applyCapturedCursorPosition(window, camera, 100.0, 80.0);
    CHECK_EQ(camera.yaw, 10.0f);
    CHECK_EQ(camera.pitch, 5.0f);
    CHECK(!window.firstMouse);
    CHECK_EQ(window.lastMouseX, 100.0);
    CHECK_EQ(window.lastMouseY, 80.0);

    applyCapturedCursorPosition(window, camera, 120.0, 70.0);
    CHECK_EQ(camera.yaw, 20.0f);
    CHECK_EQ(camera.pitch, 10.0f);
    CHECK_EQ(window.lastMouseX, 120.0);
    CHECK_EQ(window.lastMouseY, 70.0);
}

TEST_CASE(CursorLock_UncapturedSamplesDoNotRotate) {
    WindowState window;
    window.cursorCaptured = false;
    window.firstMouse = false;
    window.lastMouseX = 40.0;
    window.lastMouseY = 40.0;
    CameraState camera;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;

    applyCapturedCursorPosition(window, camera, 80.0, 10.0);
    CHECK_EQ(camera.yaw, 0.0f);
    CHECK_EQ(camera.pitch, 0.0f);
    CHECK_EQ(window.lastMouseX, 40.0);
    CHECK_EQ(window.lastMouseY, 40.0);
}

TEST_CASE(CursorLock_PitchIsClamped) {
    WindowState window;
    window.firstMouse = false;
    window.lastMouseX = 0.0;
    window.lastMouseY = 0.0;
    CameraState camera;
    camera.yaw = 0.0f;
    camera.pitch = 80.0f;
    camera.mouseSensitivity = 1.0f;

    applyCapturedCursorPosition(window, camera, 0.0, -40.0);
    CHECK_EQ(camera.pitch, 89.0f);

    applyCapturedCursorPosition(window, camera, 0.0, 200.0);
    CHECK_EQ(camera.pitch, -89.0f);
}

TEST_CASE(CursorLock_CaptureWithoutWindowOnlyUpdatesState) {
    WindowState window;
    window.cursorCaptured = false;
    window.firstMouse = false;

    setCursorCaptured(window, true);
    CHECK(window.cursorCaptured);
    CHECK(window.firstMouse);

    maintainCursorLock(window);
    CHECK(window.cursorCaptured);
    CHECK(window.firstMouse);

    setCursorCaptured(window, false);
    CHECK(!window.cursorCaptured);
    CHECK(window.firstMouse);
}
