#include "TestFramework.h"

#include "Rigel/input/keypress.h"
#include <GLFW/glfw3.h>

using namespace Rigel::Input;

TEST_CASE(Keypress_Transitions) {
    keyCallback(nullptr, GLFW_KEY_A, 0, GLFW_PRESS, 0);
    keyupdate();

    CHECK(isKeyPressed(GLFW_KEY_A));
    CHECK(isKeyJustPressed(GLFW_KEY_A));

    keyupdate();
    CHECK(isKeyPressed(GLFW_KEY_A));
    CHECK(!isKeyJustPressed(GLFW_KEY_A));

    keyCallback(nullptr, GLFW_KEY_A, 0, GLFW_RELEASE, 0);
    keyupdate();
    CHECK(!isKeyPressed(GLFW_KEY_A));
    CHECK(isKeyJustReleased(GLFW_KEY_A));
}
