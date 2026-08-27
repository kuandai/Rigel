#include "TestFramework.h"
#include "LogCapture.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/input/InputState.h"
#include "Rigel/input/GameplayInput.h"

#include <GLFW/glfw3.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace Rigel::Input;

namespace {

class ThrowingInputLoader final : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override {
        return "input";
    }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext& context) override {
        ++loadCount;
        throw std::runtime_error(
            "injected input failure for " + context.id);
    }

    size_t loadCount = 0;
};

struct RecordingListener : InputListener {
    std::vector<std::string> pressed;
    std::vector<std::string> released;

    void onActionPressed(std::string_view action) override {
        pressed.emplace_back(action);
    }

    void onActionReleased(std::string_view action) override {
        released.emplace_back(action);
    }
};

} // namespace

TEST_CASE(InputState_KeyTransitionsAreFrameScoped) {
    InputState input;

    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    CHECK(!input.isKeyPressed(GLFW_KEY_A));

    input.beginFrame();
    CHECK(input.isKeyPressed(GLFW_KEY_A));
    CHECK(input.isKeyJustPressed(GLFW_KEY_A));
    CHECK(!input.isKeyJustReleased(GLFW_KEY_A));

    input.beginFrame();
    CHECK(input.isKeyPressed(GLFW_KEY_A));
    CHECK(!input.isKeyJustPressed(GLFW_KEY_A));

    input.handleKeyEvent(GLFW_KEY_A, GLFW_REPEAT);
    input.beginFrame();
    CHECK(input.isKeyRepeating(GLFW_KEY_A));

    input.handleKeyEvent(GLFW_KEY_A, GLFW_RELEASE);
    input.beginFrame();
    CHECK(!input.isKeyPressed(GLFW_KEY_A));
    CHECK(!input.isKeyRepeating(GLFW_KEY_A));
    CHECK(input.isKeyJustReleased(GLFW_KEY_A));

    input.beginFrame();
    CHECK(!input.isKeyJustReleased(GLFW_KEY_A));

    input.handleKeyEvent(-1, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_LAST + 1, GLFW_PRESS);
    CHECK(!input.isKeyPressed(-1));
    CHECK(!input.isKeyPressed(GLFW_KEY_LAST + 1));
}

TEST_CASE(InputState_MouseTransitionsAreFrameScoped) {
    InputState input;

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    input.beginFrame();
    CHECK(input.isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_LEFT));

    input.beginFrame();
    CHECK(!input.isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_LEFT));

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
    input.beginFrame();
    CHECK(!input.isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_LEFT));

    input.handleMouseButtonEvent(-1, GLFW_PRESS);
    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LAST + 1, GLFW_PRESS);
    input.beginFrame();
    CHECK(!input.isMouseButtonJustPressed(-1));
    CHECK(!input.isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_LAST + 1));
}

TEST_CASE(InputState_ActionMappingUsesOwnedFrameState) {
    auto bindings = std::make_shared<InputBindings>();
    bindings->bind("test_action", GLFW_KEY_A);

    InputState input;
    RecordingListener listener;
    input.setBindings(bindings);
    input.addListener(&listener);
    input.beginFrame();

    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(input.isActionJustPressed("test_action"));
    CHECK_EQ(listener.pressed.size(), static_cast<std::size_t>(1));
    CHECK_EQ(listener.pressed.front(), std::string("test_action"));

    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(!input.isActionJustPressed("test_action"));
    CHECK_EQ(listener.pressed.size(), static_cast<std::size_t>(1));

    input.handleKeyEvent(GLFW_KEY_A, GLFW_RELEASE);
    input.beginFrame();
    CHECK(input.isActionJustReleased("test_action"));
    CHECK_EQ(listener.released.size(), static_cast<std::size_t>(1));
    CHECK_EQ(listener.released.front(), std::string("test_action"));
}

TEST_CASE(InputState_ActionAlternativesAggregateHeldAndEdges) {
    auto bindings = std::make_shared<InputBindings>();
    bindings->setBindings(
        "test_action",
        {{PhysicalInputType::Keyboard, GLFW_KEY_A},
         {PhysicalInputType::MouseButton, GLFW_MOUSE_BUTTON_LEFT}});

    InputState input;
    input.setBindings(bindings);
    input.beginFrame();

    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(input.isActionJustPressed("test_action"));

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(!input.isActionJustPressed("test_action"));

    input.handleKeyEvent(GLFW_KEY_A, GLFW_RELEASE);
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(!input.isActionJustReleased("test_action"));

    input.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
    input.beginFrame();
    CHECK(!input.isActionPressed("test_action"));
    CHECK(input.isActionJustReleased("test_action"));
}

TEST_CASE(InputState_CompleteTapBetweenFramesKeepsBothSemanticEdges) {
    auto bindings = std::make_shared<InputBindings>();
    bindings->bind("test_action", GLFW_KEY_A);
    InputState input;
    input.setBindings(bindings);
    input.beginFrame();

    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_A, GLFW_RELEASE);
    input.beginFrame();

    CHECK(!input.isActionPressed("test_action"));
    CHECK(input.isActionJustPressed("test_action"));
    CHECK(input.isActionJustReleased("test_action"));
}

TEST_CASE(InputState_InitialBindingInstallPreservesEventsAfterQueue) {
    auto bindings = std::make_shared<InputBindings>();
    bindings->bind("pressed_action", GLFW_KEY_A);
    bindings->bind("tapped_action", GLFW_KEY_B);
    InputState input;

    input.setBindings(bindings);
    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_B, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_B, GLFW_RELEASE);
    input.beginFrame();

    CHECK(input.isActionPressed("pressed_action"));
    CHECK(input.isActionJustPressed("pressed_action"));
    CHECK(!input.isActionPressed("tapped_action"));
    CHECK(input.isActionJustPressed("tapped_action"));
    CHECK(input.isActionJustReleased("tapped_action"));
}

TEST_CASE(InputState_BindingSwapPreservesEventsAfterQueue) {
    auto first = std::make_shared<InputBindings>();
    first->bind("changed_action", GLFW_KEY_A);
    first->bind("unchanged_action", GLFW_KEY_C);
    first->bind("released_action", GLFW_KEY_D);
    first->bind("held_action", GLFW_KEY_E);
    auto second = std::make_shared<InputBindings>();
    second->bind("changed_action", GLFW_KEY_B);
    second->bind("unchanged_action", GLFW_KEY_C);
    second->bind("released_action", GLFW_KEY_D);
    second->bind("held_action", GLFW_KEY_E);
    InputState input;
    input.setBindings(first);
    input.beginFrame();
    input.handleKeyEvent(GLFW_KEY_D, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_E, GLFW_PRESS);
    input.beginFrame();
    input.beginFrame();

    input.setBindings(second);
    input.handleKeyEvent(GLFW_KEY_B, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_C, GLFW_PRESS);
    input.handleKeyEvent(GLFW_KEY_C, GLFW_RELEASE);
    input.handleKeyEvent(GLFW_KEY_D, GLFW_RELEASE);
    input.beginFrame();

    CHECK(input.isActionPressed("changed_action"));
    CHECK(input.isActionJustPressed("changed_action"));
    CHECK(!input.isActionPressed("unchanged_action"));
    CHECK(input.isActionJustPressed("unchanged_action"));
    CHECK(input.isActionJustReleased("unchanged_action"));
    CHECK(!input.isActionPressed("released_action"));
    CHECK(input.isActionJustReleased("released_action"));
    CHECK(input.isActionPressed("held_action"));
    CHECK(!input.isActionJustPressed("held_action"));
    CHECK(!input.isActionJustReleased("held_action"));
}

TEST_CASE(InputState_BindingSwapRebasesHeldStateWithoutSyntheticEdges) {
    auto first = std::make_shared<InputBindings>();
    first->bind("test_action", GLFW_KEY_A);
    auto second = std::make_shared<InputBindings>();
    second->bind("test_action", GLFW_KEY_B);

    InputState input;
    input.setBindings(first);
    input.beginFrame();
    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    input.beginFrame();
    CHECK(input.isActionJustPressed("test_action"));

    input.handleKeyEvent(GLFW_KEY_B, GLFW_PRESS);
    input.setBindings(second);
    CHECK(input.isActionPressed("test_action"));
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(!input.isActionJustPressed("test_action"));
    CHECK(!input.isActionJustReleased("test_action"));

    input.handleKeyEvent(GLFW_KEY_A, GLFW_RELEASE);
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));
    CHECK(!input.isActionJustReleased("test_action"));

    input.handleKeyEvent(GLFW_KEY_B, GLFW_RELEASE);
    input.beginFrame();
    CHECK(input.isActionJustReleased("test_action"));
}

TEST_CASE(InputState_FocusLossReleasesHeldSemanticActions) {
    auto bindings = std::make_shared<InputBindings>();
    bindings->bind("test_action", GLFW_KEY_A);
    InputState input;
    input.setBindings(bindings);
    input.beginFrame();
    input.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    input.beginFrame();
    CHECK(input.isActionPressed("test_action"));

    input.handleFocusLost();
    input.beginFrame();
    CHECK(!input.isActionPressed("test_action"));
    CHECK(input.isActionJustReleased("test_action"));

    input.beginFrame();
    CHECK(!input.isActionJustReleased("test_action"));
}

TEST_CASE(InputState_InstancesDoNotShareDeviceState) {
    InputState first;
    InputState second;

    first.handleKeyEvent(GLFW_KEY_A, GLFW_PRESS);
    first.handleMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    first.beginFrame();
    second.beginFrame();

    CHECK(first.isKeyPressed(GLFW_KEY_A));
    CHECK(first.isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_LEFT));
    CHECK(!second.isKeyPressed(GLFW_KEY_A));
    CHECK(!second.isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_LEFT));
}

TEST_CASE(InputBindings_ShippedDefaultsAreCompleteAndCacheIsImmutable) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    assets.registerLoader(
        "input", std::make_unique<Rigel::Input::InputBindingsLoader>());
    const auto defaults = loadPlayerDefaultBindings(assets);

    CHECK_EQ(defaults->bindings().size(), Rigel::Preferences::kUserActions.size());
    for (const auto& [action, name] : Rigel::Preferences::kUserActions) {
        static_cast<void>(action);
        CHECK(defaults->isBound(name));
    }
    CHECK(!defaults->hasAction("debug_overlay"));
    CHECK_EQ(
        defaults->bindings().at("remove_block"),
        (std::vector<PhysicalInput>{{
            PhysicalInputType::MouseButton, GLFW_MOUSE_BUTTON_LEFT}}));

    Rigel::Preferences::InputPreferences preferences;
    preferences.bindings[Rigel::Preferences::UserAction::MoveForward] =
        {"UP", "MOUSE_4"};
    preferences.bindings[Rigel::Preferences::UserAction::PlaceBlock] = {};
    const auto effective = compileInputBindings(*defaults, preferences);

    CHECK_EQ(
        effective->bindings().at("move_forward"),
        (std::vector<PhysicalInput>{
            {PhysicalInputType::Keyboard, GLFW_KEY_UP},
            {PhysicalInputType::MouseButton, GLFW_MOUSE_BUTTON_4}}));
    CHECK(!effective->isBound("place_block"));
    CHECK_EQ(
        defaults->bindings().at("move_forward"),
        (std::vector<PhysicalInput>{{
            PhysicalInputType::Keyboard, GLFW_KEY_W}}));
    CHECK(defaults->isBound("place_block"));
    CHECK(!defaults->hasAction("debug_overlay"));
    CHECK_EQ(
        effective->bindings().at("debug_overlay"),
        (std::vector<PhysicalInput>{{
            PhysicalInputType::Keyboard, GLFW_KEY_F1}}));
}

TEST_CASE(InputBindings_SymbolicDecoderRejectsNumericAndUnknownTokens) {
    CHECK_EQ(
        decodeBindingToken("left-shift"),
        std::optional<PhysicalInput>({
            PhysicalInputType::Keyboard, GLFW_KEY_LEFT_SHIFT}));
    CHECK_EQ(
        decodeBindingToken("MOUSE_RIGHT"),
        std::optional<PhysicalInput>({
            PhysicalInputType::MouseButton, GLFW_MOUSE_BUTTON_RIGHT}));
    CHECK(!decodeBindingToken("87"));
    CHECK(!decodeBindingToken("-1"));
    CHECK(!decodeBindingToken("+1"));
    CHECK(!decodeBindingToken("  -1 "));
    CHECK(!decodeBindingToken("999999999999999999999999999999"));
    CHECK(!decodeBindingToken("MOUSE_9"));
    CHECK(!decodeBindingToken("NOT_A_KEY"));
}

TEST_CASE(InputBindings_InvalidOrIncompleteDefaultsFailWithoutFallback) {
    InputBindings incomplete;
    incomplete.bind("move_forward", GLFW_KEY_W);
    CHECK_THROWS(compileInputBindings(
        incomplete, Rigel::Preferences::InputPreferences{}));

    {
        Rigel::Asset::AssetManager assets;
        CHECK_THROWS(loadPlayerDefaultBindings(assets));
    }

    {
        Rigel::Asset::AssetManager assets;
        auto loader = std::make_unique<ThrowingInputLoader>();
        auto* loaderProbe = loader.get();
        assets.registerLoader("input", std::move(loader));
        assets.loadManifest("manifest.yaml");

        CHECK_THROWS(loadPlayerDefaultBindings(assets));
        CHECK_EQ(loaderProbe->loadCount, static_cast<size_t>(1));
    }
}

TEST_CASE(InputBindings_DuplicatePhysicalInputsWarnOnceAndRemainAllowed) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    assets.registerLoader(
        "input", std::make_unique<Rigel::Input::InputBindingsLoader>());
    const auto defaults = loadPlayerDefaultBindings(assets);
    Rigel::Preferences::InputPreferences preferences;
    preferences.bindings[Rigel::Preferences::UserAction::MoveForward] = {"A"};

    Rigel::Test::LogCapture logs("duplicate-input-bindings");
    const auto effective = compileInputBindings(*defaults, preferences);
    CHECK(effective->isBound("move_forward"));
    CHECK(effective->isBound("move_left"));
    CHECK_EQ(
        Rigel::Test::countOccurrences(
            logs.output(), "Multiple actions share a physical input binding"),
        static_cast<size_t>(1));
}
