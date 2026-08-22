#include "TestFramework.h"
#include "LogCapture.h"

#include "Rigel/Asset/AssetManager.h"
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

TEST_CASE(InputState_DefaultBindingsCoverAbsentAndFailedAssets) {
    {
        Rigel::Test::LogCapture logs("input-optional-absent-test");
        Rigel::Asset::AssetManager assets;
        InputState input;
        loadInputBindings(assets, input);
        input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);
        input.beginFrame();
        CHECK(input.isActionPressed("move_forward"));
        CHECK_EQ(
            Rigel::Test::countOccurrences(
                logs.output(), "Optional startup resource 'input/default'"),
            static_cast<size_t>(1));
    }

    {
        Rigel::Test::LogCapture logs("input-optional-failure-test");
        Rigel::Asset::AssetManager assets;
        auto loader = std::make_unique<ThrowingInputLoader>();
        auto* loaderProbe = loader.get();
        assets.registerLoader("input", std::move(loader));
        assets.loadManifest("manifest.yaml");

        InputState input;
        CHECK_NO_THROW(loadInputBindings(assets, input));
        CHECK_EQ(loaderProbe->loadCount, static_cast<size_t>(1));
        input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);
        input.beginFrame();
        CHECK(input.isActionPressed("move_forward"));
        const auto output = logs.output();
        CHECK_EQ(
            Rigel::Test::countOccurrences(
                output, "Optional startup resource 'input/default'"),
            static_cast<size_t>(1));
        CHECK(output.find("injected input failure") != std::string::npos);
    }
}
