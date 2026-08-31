#include "TestFramework.h"

#include "ApplicationPreferences.h"
#include "DeveloperDiagnostics.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Voxel/RenderProfile.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

namespace {

template<typename T>
concept HasAntiAliasingPreference = requires(T value) {
    value.graphics.antiAliasing;
};

template<typename T>
concept HasProfilingSetting = requires(T value) {
    value.profilingEnabled;
};

template<typename T>
concept StoresDerivedRenderDistance = requires(T value) {
    value.renderDistance;
};

template<typename T>
concept HasForegroundTransparentAlpha = requires(T value) {
    value.transparentAlpha;
};

static_assert(
    !HasAntiAliasingPreference<Rigel::Preferences::UserPreferences>);
static_assert(!HasProfilingSetting<Rigel::Voxel::RenderProfile>);
static_assert(!StoresDerivedRenderDistance<Rigel::Voxel::RenderProfile>);
static_assert(!HasForegroundTransparentAlpha<Rigel::Voxel::RenderProfile>);

class ViewDistanceRenderFixture final {
public:
    explicit ViewDistanceRenderFixture(int chunks)
        : m_directory("rigel_render_profile")
        , world(resources)
        , view(world, resources)
        , preferences(m_directory.path() / "user-preferences.yaml") {
        Rigel::Preferences::UserPreferences requested;
        requested.graphics.viewDistanceChunks = chunks;
        Rigel::Preferences::UserPreferencesStore(
            m_directory.path() / "user-preferences.yaml")
            .saveRequested(requested);
        preferences.load();
        preferences.initializeViewDistance(view);
    }

private:
    Rigel::Test::TemporaryDirectory m_directory;

public:
    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world;
    Rigel::Voxel::WorldView view;
    Rigel::ApplicationPreferences preferences;
};

} // namespace

TEST_CASE(RenderProfile_UsesOneShippedShadowAndArtProfile) {
    const Rigel::Voxel::RenderProfile profile;

    CHECK_NEAR(profile.sunDirection.x, 0.5f, 0.0001f);
    CHECK_NEAR(profile.sunDirection.y, 1.0f, 0.0001f);
    CHECK_NEAR(profile.sunDirection.z, 0.3f, 0.0001f);
    CHECK_EQ(profile.shadow.cascades, 3);
    CHECK_EQ(profile.shadow.mapSize, 6144);
    CHECK_NEAR(
        profile.shadow.maximumDistanceWorldUnits, 200.0f, 0.0001f);
    CHECK_NEAR(profile.shadow.splitLambda, 0.25f, 0.0001f);
    CHECK_NEAR(profile.shadow.bias, 0.001f, 0.0001f);
    CHECK_NEAR(profile.shadow.normalBias, 0.02f, 0.0001f);
    CHECK_EQ(profile.shadow.pcfRadiusNear, 2);
    CHECK_EQ(profile.shadow.pcfRadiusFar, 3);
    CHECK_NEAR(profile.shadow.transparentScale, 1.0f, 0.0001f);
    CHECK_NEAR(profile.shadow.strength, 3.0f, 0.0001f);
    CHECK_NEAR(profile.shadow.fadePower, 1.0f, 0.0001f);
}

TEST_CASE(RenderProfile_TaaRemainsInternalAndDisabledByDefault) {
    const Rigel::Voxel::RenderProfile profile;

    CHECK(!profile.temporalAA.enabled);
    CHECK_NEAR(profile.temporalAA.blend, 0.95f, 0.0001f);
    CHECK_NEAR(profile.temporalAA.jitterScale, 1.0f, 0.0001f);
}

TEST_CASE(RenderProfile_ViewDistanceCeilingBoundsShadowsWithoutMutation) {
    ViewDistanceRenderFixture fixture(2);
    const float profileDistance =
        fixture.view.renderProfile().shadow.maximumDistanceWorldUnits;

    CHECK_NEAR(fixture.view.renderDistanceWorldUnits(), 96.0f, 0.0001f);
    CHECK_NEAR(fixture.view.shadowDistanceWorldUnits(), 96.0f, 0.0001f);
    CHECK_NEAR(
        fixture.view.renderProfile().shadow.maximumDistanceWorldUnits,
        profileDistance,
        0.0001f);
}

TEST_CASE(RenderProfile_InternalDistanceCapsLargerViewDistance) {
    ViewDistanceRenderFixture fixture(12);

    CHECK_NEAR(fixture.view.renderDistanceWorldUnits(), 416.0f, 0.0001f);
    CHECK_NEAR(fixture.view.shadowDistanceWorldUnits(), 200.0f, 0.0001f);
}

TEST_CASE(DeveloperDiagnostics_ProfilerRequiresExplicitEnableValue) {
    using Rigel::detail::profilerEnabledFromEnvironmentValue;

    CHECK(!profilerEnabledFromEnvironmentValue(nullptr));
    CHECK(!profilerEnabledFromEnvironmentValue(""));
    CHECK(!profilerEnabledFromEnvironmentValue("0"));
    CHECK(!profilerEnabledFromEnvironmentValue("true"));
    CHECK(profilerEnabledFromEnvironmentValue("1"));
}
