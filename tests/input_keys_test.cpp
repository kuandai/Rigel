#include "TestFramework.h"

#include "Rigel/input/keys.h"
#include <GLFW/glfw3.h>

using namespace Rigel;

TEST_CASE(KeyNames_KnownAndInvalid) {
    CHECK_EQ(std::string(getKeyName(GLFW_KEY_SPACE)), "SPACE");
    CHECK_EQ(std::string(getKeyName(-1)), "");
}
