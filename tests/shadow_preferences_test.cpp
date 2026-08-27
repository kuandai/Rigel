#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "ApplicationPreferences.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <functional>

namespace Rigel::Preferences::detail {

void setUserPreferencesBeforePublicationHookForTesting(
    std::function<void()> hook);

} // namespace Rigel::Preferences::detail

namespace Rigel::Voxel::detail {

struct WorldViewTestAccess {
    struct ShadowResources {
        GLuint depthArray = 0;
        GLuint transmitArray = 0;
        GLuint framebuffer = 0;
        int cascades = 0;
        int mapSize = 0;
    };

    static ShadowResources shadowResources(const WorldView& view) {
        const auto state = view.m_renderer.shadowRenderState();
        return {
            state.depthArray,
            state.transmitArray,
            view.m_renderer.m_shadowState.fbo,
            state.cascades,
            state.mapSize,
        };
    }
};

} // namespace Rigel::Voxel::detail

namespace {

void failBeforePublication() {
    throw Rigel::Persistence::AtomicFilePublicationError(
        Rigel::Persistence::AtomicFilePublicationState::NotPublished,
        "injected prepublication failure");
}

class ShadowFixture final {
private:
    Rigel::Test::TemporaryDirectory m_directory;

public:
    ShadowFixture()
        : m_directory("rigel_shadow_preferences")
        , world(resources)
        , view(world, resources)
        , path(m_directory.path() / "user-preferences.yaml") {
        context.require();
        assets.loadManifest("manifest.yaml");
        view.initialize(assets);

        Rigel::Voxel::WorldRenderConfig profile;
        profile.shadow.cascades = 2;
        profile.shadow.mapSize = 32;
        view.setRenderConfig(profile);
    }

    ~ShadowFixture() {
        Rigel::Preferences::detail::
            setUserPreferencesBeforePublicationHookForTesting({});
        view.releaseRenderResources();
        assets.clearCache();
    }

    Rigel::ApplicationPreferences owner(bool shadows) {
        Rigel::Preferences::UserPreferences requested;
        requested.graphics.shadows = shadows;
        Rigel::Preferences::UserPreferencesStore(path).saveRequested(requested);
        Rigel::ApplicationPreferences preferences(path);
        preferences.load();
        return preferences;
    }

    Rigel::Test::HiddenOpenGLContext context;
    Rigel::Asset::AssetManager assets;
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world;
    Rigel::Voxel::WorldView view;
    std::filesystem::path path;
};

using ShadowResources =
    Rigel::Voxel::detail::WorldViewTestAccess::ShadowResources;

ShadowResources shadowResources(const Rigel::Voxel::WorldView& view) {
    return Rigel::Voxel::detail::WorldViewTestAccess::shadowResources(view);
}

bool persistedShadows(const std::filesystem::path& path) {
    return Rigel::Preferences::UserPreferencesStore(path)
        .load()
        .graphics.shadows;
}

} // namespace

TEST_CASE(ApplicationPreferences_ShadowStartupAndLiveToggleOwnResources) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(true);

    const auto initialized = preferences.initializeShadows(fixture.view);

    CHECK_EQ(initialized.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK(preferences.effectiveShadowsEnabled());
    const ShadowResources enabled = shadowResources(fixture.view);
    CHECK(enabled.depthArray != 0);
    CHECK(enabled.transmitArray != 0);
    CHECK(enabled.framebuffer != 0);
    CHECK_EQ(enabled.cascades, 2);
    CHECK_EQ(enabled.mapSize, 32);
    CHECK_EQ(glIsTexture(enabled.depthArray), GL_TRUE);
    CHECK_EQ(glIsTexture(enabled.transmitArray), GL_TRUE);
    CHECK_EQ(glIsFramebuffer(enabled.framebuffer), GL_TRUE);

    const auto disabled = preferences.applyShadows(fixture.view, false);

    CHECK_EQ(disabled.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK(!persistedShadows(fixture.path));
    const ShadowResources released = shadowResources(fixture.view);
    CHECK_EQ(released.depthArray, static_cast<GLuint>(0));
    CHECK_EQ(released.transmitArray, static_cast<GLuint>(0));
    CHECK_EQ(released.framebuffer, static_cast<GLuint>(0));
    CHECK_EQ(glIsTexture(enabled.depthArray), GL_FALSE);
    CHECK_EQ(glIsTexture(enabled.transmitArray), GL_FALSE);
    CHECK_EQ(glIsFramebuffer(enabled.framebuffer), GL_FALSE);

    const auto reenabled = preferences.applyShadows(fixture.view, true);

    CHECK_EQ(reenabled.status, Rigel::PreferenceApplyStatus::Applied);
    CHECK(preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK(persistedShadows(fixture.path));
    CHECK(shadowResources(fixture.view).depthArray != 0);
}

TEST_CASE(ApplicationPreferences_UnpublishedShadowDisableRestoresResources) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(true);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    const ShadowResources before = shadowResources(fixture.view);
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);

    const auto result = preferences.applyShadows(fixture.view, false);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK(preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK(persistedShadows(fixture.path));
    const ShadowResources restored = shadowResources(fixture.view);
    CHECK_EQ(restored.depthArray, before.depthArray);
    CHECK_EQ(restored.transmitArray, before.transmitArray);
    CHECK_EQ(restored.framebuffer, before.framebuffer);
    CHECK_EQ(glIsTexture(restored.depthArray), GL_TRUE);
    CHECK_EQ(glIsTexture(restored.transmitArray), GL_TRUE);
    CHECK_EQ(glIsFramebuffer(restored.framebuffer), GL_TRUE);
}

TEST_CASE(ApplicationPreferences_UnpublishedShadowEnableRestoresOffState) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(false);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            &failBeforePublication);

    const auto result = preferences.applyShadows(fixture.view, true);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK(!persistedShadows(fixture.path));
    const ShadowResources restored = shadowResources(fixture.view);
    CHECK_EQ(restored.depthArray, static_cast<GLuint>(0));
    CHECK_EQ(restored.transmitArray, static_cast<GLuint>(0));
    CHECK_EQ(restored.framebuffer, static_cast<GLuint>(0));
}

TEST_CASE(ApplicationPreferences_InvalidShadowProfileRetainsOffState) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(false);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    Rigel::Voxel::WorldRenderConfig invalid = fixture.view.renderConfig();
    invalid.shadow.mapSize = Rigel::Voxel::ShadowConfig::MaxMapSize + 1;
    fixture.view.setRenderConfig(invalid);

    const auto result = preferences.applyShadows(fixture.view, true);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK(!persistedShadows(fixture.path));
    CHECK_EQ(
        shadowResources(fixture.view).depthArray,
        static_cast<GLuint>(0));
}

TEST_CASE(ApplicationPreferences_UnavailableShadowRendererRetainsRequest) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_unavailable_shadow_renderer");
    const auto path = directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferences requested;
    requested.graphics.shadows = false;
    Rigel::Preferences::UserPreferencesStore(path).saveRequested(requested);
    Rigel::ApplicationPreferences preferences(path);
    preferences.load();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);
    Rigel::Voxel::WorldView view(world, resources);
    CHECK_EQ(
        preferences.initializeShadows(view).status,
        Rigel::PreferenceApplyStatus::Applied);

    const auto result = preferences.applyShadows(view, true);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK(!persistedShadows(path));
}

TEST_CASE(ApplicationPreferences_ShadowStartupFailureRetainsPersistedOn) {
    Rigel::Test::TemporaryDirectory directory(
        "rigel_unavailable_startup_shadow_renderer");
    const auto path = directory.path() / "user-preferences.yaml";
    Rigel::Preferences::UserPreferences requested;
    requested.graphics.shadows = true;
    Rigel::Preferences::UserPreferencesStore(path).saveRequested(requested);
    Rigel::ApplicationPreferences preferences(path);
    preferences.load();
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);
    Rigel::Voxel::WorldView view(world, resources);

    const auto result = preferences.initializeShadows(view);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK(persistedShadows(path));
}
