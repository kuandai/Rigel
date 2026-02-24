#include "TestFramework.h"

#include "Rigel/input/GameplayInput.h"

#include <GLFW/glfw3.h>

using namespace Rigel::Input;

TEST_CASE(GameplayInput_EnsureDefaultBindings_SetsDebugNearTerrainAction) {
    InputBindings bindings;
    ensureDefaultBindings(bindings);

    CHECK(bindings.hasAction("debug_toggle_near_terrain"));
    CHECK(bindings.isBound("debug_toggle_near_terrain"));
    auto key = bindings.keyFor("debug_toggle_near_terrain");
    CHECK(key.has_value());
    CHECK_EQ(*key, GLFW_KEY_F4);
}

TEST_CASE(GameplayInput_EnsureDefaultBindings_DoesNotOverrideExistingDebugNearTerrainBinding) {
    InputBindings bindings;
    bindings.bind("debug_toggle_near_terrain", GLFW_KEY_F8);
    ensureDefaultBindings(bindings);

    auto key = bindings.keyFor("debug_toggle_near_terrain");
    CHECK(key.has_value());
    CHECK_EQ(*key, GLFW_KEY_F8);
}
