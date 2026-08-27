#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "ApplicationPreferences.h"
#include "Rigel/Asset/AssetLoader.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <array>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string_view>

namespace Rigel::Preferences::detail {

void setUserPreferencesAfterSavePreflightHookForTesting(
    std::function<void()> hook);
void setUserPreferencesBeforePublicationHookForTesting(
    std::function<void()> hook);
void setUserPreferencesAfterPublicationHookForTesting(
    std::function<void()> hook);

} // namespace Rigel::Preferences::detail

namespace Rigel::Voxel::detail {

struct WorldViewTestAccess {
    using ShadowPreparationHook = std::function<std::optional<GLenum>(
        std::string_view, GLuint, GLuint, GLuint)>;

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

    static void setShadowPreparationHook(
        WorldView& view,
        ShadowPreparationHook hook) {
        view.m_renderer.m_shadowPreparationHookForTesting = std::move(hook);
    }
};

} // namespace Rigel::Voxel::detail

namespace {

class MissingShadowDepthShaderLoader final
    : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext& context) override {
        if (context.id == "shaders/voxel_shadow_depth") {
            throw std::runtime_error(
                "injected voxel shadow depth shader failure");
        }
        return std::make_shared<Rigel::Asset::ShaderAsset>();
    }
};

void failBeforePublication() {
    throw Rigel::Persistence::AtomicFilePublicationError(
        Rigel::Persistence::AtomicFilePublicationState::NotPublished,
        "injected prepublication failure");
}

void failSavePreflight() {
    throw std::runtime_error("injected save preflight failure");
}

void failAfterPublication() {
    throw Rigel::Persistence::AtomicFilePublicationError(
        Rigel::Persistence::AtomicFilePublicationState::
            PublishedDurabilityUncertain,
        "injected post-publication durability uncertainty");
}

#ifndef _WIN32
class DirectoryWriteBlock final {
public:
    explicit DirectoryWriteBlock(std::filesystem::path path)
        : m_path(std::move(path))
        , m_original(std::filesystem::status(m_path).permissions()) {
    }

    ~DirectoryWriteBlock() { restore(); }

    void block() {
        std::filesystem::permissions(
            m_path,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace);
        m_blocked = true;
    }

    bool blocked() const noexcept { return m_blocked; }

    void restore() noexcept {
        if (!m_blocked) {
            return;
        }
        std::error_code error;
        std::filesystem::permissions(
            m_path,
            m_original,
            std::filesystem::perm_options::replace,
            error);
        m_blocked = false;
    }

private:
    std::filesystem::path m_path;
    std::filesystem::perms m_original;
    bool m_blocked = false;
};
#endif

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
        Rigel::Voxel::detail::WorldViewTestAccess::
            setShadowPreparationHook(view, {});
        Rigel::Preferences::detail::
            setUserPreferencesAfterSavePreflightHookForTesting({});
        Rigel::Preferences::detail::
            setUserPreferencesBeforePublicationHookForTesting({});
        Rigel::Preferences::detail::
            setUserPreferencesAfterPublicationHookForTesting({});
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

class ShadowPreparationBindings final {
public:
    ShadowPreparationBindings() {
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &m_previousTexture);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_previousDrawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_previousReadFramebuffer);

        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_texture);
        glGenFramebuffers(1, &m_drawFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_drawFramebuffer);
        glGenFramebuffers(1, &m_readFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFramebuffer);
    }

    ~ShadowPreparationBindings() {
        glBindTexture(
            GL_TEXTURE_2D_ARRAY,
            static_cast<GLuint>(m_previousTexture));
        glBindFramebuffer(
            GL_DRAW_FRAMEBUFFER,
            static_cast<GLuint>(m_previousDrawFramebuffer));
        glBindFramebuffer(
            GL_READ_FRAMEBUFFER,
            static_cast<GLuint>(m_previousReadFramebuffer));
        glDeleteTextures(1, &m_texture);
        glDeleteFramebuffers(1, &m_drawFramebuffer);
        glDeleteFramebuffers(1, &m_readFramebuffer);
    }

    GLuint texture() const { return m_texture; }
    GLuint drawFramebuffer() const { return m_drawFramebuffer; }
    GLuint readFramebuffer() const { return m_readFramebuffer; }

private:
    GLint m_previousTexture = 0;
    GLint m_previousDrawFramebuffer = 0;
    GLint m_previousReadFramebuffer = 0;
    GLuint m_texture = 0;
    GLuint m_drawFramebuffer = 0;
    GLuint m_readFramebuffer = 0;
};

enum class ShadowPreparationFailureKind {
    Throw,
    OpenGLError,
    IncompleteFramebuffer,
};

struct ShadowPreparationFailureCase {
    std::string_view phase;
    ShadowPreparationFailureKind kind;
    std::string_view expectedMessage;
    bool hasDepth;
    bool hasTransmittance;
    bool hasFramebuffer;
};

constexpr std::array<ShadowPreparationFailureCase, 8>
    kShadowPreparationFailureCases{{
        {"depth_texture_created",
         ShadowPreparationFailureKind::Throw,
         "injected shadow preparation failure",
         true,
         false,
         false},
        {"depth_texture_allocated",
         ShadowPreparationFailureKind::OpenGLError,
         "OpenGL rejected shadow resource depth allocation",
         true,
         false,
         false},
        {"transmittance_texture_created",
         ShadowPreparationFailureKind::Throw,
         "injected shadow preparation failure",
         true,
         true,
         false},
        {"transmittance_texture_allocated",
         ShadowPreparationFailureKind::OpenGLError,
         "OpenGL rejected shadow resource transmittance allocation",
         true,
         true,
         false},
        {"framebuffer_created",
         ShadowPreparationFailureKind::Throw,
         "injected shadow preparation failure",
         true,
         true,
         true},
        {"depth_framebuffer_completeness",
         ShadowPreparationFailureKind::IncompleteFramebuffer,
         "shadow depth framebuffer validation failed",
         true,
         true,
         true},
        {"transmittance_framebuffer_completeness",
         ShadowPreparationFailureKind::IncompleteFramebuffer,
         "shadow transmittance framebuffer validation failed",
         true,
         true,
         true},
        {"final_framebuffer_validation",
         ShadowPreparationFailureKind::OpenGLError,
         "OpenGL rejected shadow resource framebuffer validation",
         true,
         true,
         true},
    }};

struct ObservedShadowPreparation {
    bool reached = false;
    bool depthWasObject = false;
    bool transmittanceWasObject = false;
    bool framebufferWasObject = false;
    GLuint depth = 0;
    GLuint transmittance = 0;
    GLuint framebuffer = 0;
};

class MissingShadowDepthFixture final {
private:
    Rigel::Test::TemporaryDirectory m_directory;

public:
    MissingShadowDepthFixture()
        : m_directory("rigel_missing_shadow_depth_shader")
        , world(resources)
        , view(world, resources)
        , path(m_directory.path() / "user-preferences.yaml") {
        assets.registerLoader(
            "shaders",
            std::make_unique<MissingShadowDepthShaderLoader>());
        assets.loadManifest("manifest.yaml");
        view.initialize(assets);
    }

    ~MissingShadowDepthFixture() {
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

std::string persistedDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
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

TEST_CASE(ApplicationPreferences_PartialShadowPreparationFailureIsAtomic) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(false);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    const ShadowResources beforeResources = shadowResources(fixture.view);
    const std::string beforeDocument = persistedDocument(fixture.path);

    for (const auto& failure : kShadowPreparationFailureCases) {
        ShadowPreparationBindings bindings;
        ObservedShadowPreparation observed;
        Rigel::Voxel::detail::WorldViewTestAccess::setShadowPreparationHook(
            fixture.view,
            [&](std::string_view phase,
                GLuint depth,
                GLuint transmittance,
                GLuint framebuffer) -> std::optional<GLenum> {
                if (phase != failure.phase) {
                    return std::nullopt;
                }
                observed.reached = true;
                observed.depth = depth;
                observed.transmittance = transmittance;
                observed.framebuffer = framebuffer;
                observed.depthWasObject = glIsTexture(depth) == GL_TRUE;
                observed.transmittanceWasObject =
                    glIsTexture(transmittance) == GL_TRUE;
                observed.framebufferWasObject =
                    glIsFramebuffer(framebuffer) == GL_TRUE;

                switch (failure.kind) {
                    case ShadowPreparationFailureKind::Throw:
                        throw std::runtime_error(
                            "injected shadow preparation failure");
                    case ShadowPreparationFailureKind::OpenGLError:
                        glBindFramebuffer(GL_TEXTURE_2D, 0);
                        return std::nullopt;
                    case ShadowPreparationFailureKind::
                        IncompleteFramebuffer:
                        return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
                }
                return std::nullopt;
            });

        const auto result = preferences.applyShadows(fixture.view, true);
        Rigel::Voxel::detail::WorldViewTestAccess::
            setShadowPreparationHook(fixture.view, {});

        CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
        CHECK_NE(
            result.message.find(failure.expectedMessage),
            std::string::npos);
        CHECK(observed.reached);
        CHECK_EQ(observed.depth != 0, failure.hasDepth);
        CHECK_EQ(
            observed.transmittance != 0,
            failure.hasTransmittance);
        CHECK_EQ(observed.framebuffer != 0, failure.hasFramebuffer);
        CHECK_EQ(observed.depthWasObject, failure.hasDepth);
        CHECK_EQ(
            observed.transmittanceWasObject,
            failure.hasTransmittance);
        CHECK_EQ(observed.framebufferWasObject, failure.hasFramebuffer);
        if (failure.hasDepth) {
            CHECK_EQ(glIsTexture(observed.depth), GL_FALSE);
        }
        if (failure.hasTransmittance) {
            CHECK_EQ(glIsTexture(observed.transmittance), GL_FALSE);
        }
        if (failure.hasFramebuffer) {
            CHECK_EQ(glIsFramebuffer(observed.framebuffer), GL_FALSE);
        }

        GLint boundTexture = 0;
        GLint boundDrawFramebuffer = 0;
        GLint boundReadFramebuffer = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &boundTexture);
        glGetIntegerv(
            GL_DRAW_FRAMEBUFFER_BINDING,
            &boundDrawFramebuffer);
        glGetIntegerv(
            GL_READ_FRAMEBUFFER_BINDING,
            &boundReadFramebuffer);
        CHECK_EQ(
            boundTexture,
            static_cast<GLint>(bindings.texture()));
        CHECK_EQ(
            boundDrawFramebuffer,
            static_cast<GLint>(bindings.drawFramebuffer()));
        CHECK_EQ(
            boundReadFramebuffer,
            static_cast<GLint>(bindings.readFramebuffer()));
        CHECK_EQ(glIsTexture(bindings.texture()), GL_TRUE);
        CHECK_EQ(
            glIsFramebuffer(bindings.drawFramebuffer()),
            GL_TRUE);
        CHECK_EQ(
            glIsFramebuffer(bindings.readFramebuffer()),
            GL_TRUE);

        const ShadowResources retained = shadowResources(fixture.view);
        CHECK_EQ(retained.depthArray, beforeResources.depthArray);
        CHECK_EQ(
            retained.transmitArray,
            beforeResources.transmitArray);
        CHECK_EQ(retained.framebuffer, beforeResources.framebuffer);
        CHECK(!preferences.effectiveShadowsEnabled());
        CHECK(!preferences.requested().graphics.shadows);
        CHECK_EQ(persistedDocument(fixture.path), beforeDocument);
    }
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

TEST_CASE(ApplicationPreferences_ShadowEnablePreflightFailureRetainsOffState) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(false);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    const std::string before = persistedDocument(fixture.path);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(
            &failSavePreflight);

    const auto result = preferences.applyShadows(fixture.view, true);

    CHECK_EQ(
        result.status,
        Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK_EQ(persistedDocument(fixture.path), before);
    const ShadowResources resources = shadowResources(fixture.view);
    CHECK_EQ(resources.depthArray, static_cast<GLuint>(0));
    CHECK_EQ(resources.transmitArray, static_cast<GLuint>(0));
    CHECK_EQ(resources.framebuffer, static_cast<GLuint>(0));
}

TEST_CASE(ApplicationPreferences_ShadowDisablePreflightFailureRetainsResources) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(true);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    const ShadowResources beforeResources = shadowResources(fixture.view);
    const std::string beforeDocument = persistedDocument(fixture.path);
    Rigel::Preferences::detail::
        setUserPreferencesAfterSavePreflightHookForTesting(
            &failSavePreflight);

    const auto result = preferences.applyShadows(fixture.view, false);

    CHECK_EQ(
        result.status,
        Rigel::PreferenceApplyStatus::PersistenceBlocked);
    CHECK(preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK_EQ(persistedDocument(fixture.path), beforeDocument);
    const ShadowResources retained = shadowResources(fixture.view);
    CHECK_EQ(retained.depthArray, beforeResources.depthArray);
    CHECK_EQ(retained.transmitArray, beforeResources.transmitArray);
    CHECK_EQ(retained.framebuffer, beforeResources.framebuffer);
    CHECK_EQ(glIsTexture(retained.depthArray), GL_TRUE);
    CHECK_EQ(glIsTexture(retained.transmitArray), GL_TRUE);
    CHECK_EQ(glIsFramebuffer(retained.framebuffer), GL_TRUE);
}

TEST_CASE(ApplicationPreferences_ShadowStorageFailureRestoresResources) {
#ifdef _WIN32
    throw Rigel::Test::TestSkip(
        "Directory write permission behavior is platform-specific");
#else
    ShadowFixture fixture;
    auto preferences = fixture.owner(true);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    const ShadowResources beforeResources = shadowResources(fixture.view);
    const std::string beforeDocument = persistedDocument(fixture.path);
    DirectoryWriteBlock writeBlock(fixture.path.parent_path());
    bool observedInstalledOffState = false;
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting(
            [&]() {
                const ShadowResources installed =
                    shadowResources(fixture.view);
                observedInstalledOffState = installed.depthArray == 0 &&
                    installed.transmitArray == 0 &&
                    installed.framebuffer == 0;
                writeBlock.block();
            });

    const auto result = preferences.applyShadows(fixture.view, false);

    const bool physicalWriteWasBlocked = writeBlock.blocked();
    Rigel::Preferences::detail::
        setUserPreferencesBeforePublicationHookForTesting({});
    writeBlock.restore();
    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::NotPublished);
    CHECK(observedInstalledOffState);
    CHECK(physicalWriteWasBlocked);
    CHECK(preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK_EQ(persistedDocument(fixture.path), beforeDocument);
    const ShadowResources restored = shadowResources(fixture.view);
    CHECK_EQ(restored.depthArray, beforeResources.depthArray);
    CHECK_EQ(restored.transmitArray, beforeResources.transmitArray);
    CHECK_EQ(restored.framebuffer, beforeResources.framebuffer);
    CHECK_EQ(glIsTexture(restored.depthArray), GL_TRUE);
    CHECK_EQ(glIsTexture(restored.transmitArray), GL_TRUE);
    CHECK_EQ(glIsFramebuffer(restored.framebuffer), GL_TRUE);
#endif
}

TEST_CASE(ApplicationPreferences_UncertainShadowEnableKeepsPublishedResources) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(false);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    Rigel::Preferences::detail::
        setUserPreferencesAfterPublicationHookForTesting(
            &failAfterPublication);

    const auto result = preferences.applyShadows(fixture.view, true);

    CHECK_EQ(
        result.status,
        Rigel::PreferenceApplyStatus::PublishedDurabilityUncertain);
    CHECK(preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK(persistedShadows(fixture.path));
    const ShadowResources installed = shadowResources(fixture.view);
    CHECK(installed.depthArray != 0);
    CHECK(installed.transmitArray != 0);
    CHECK(installed.framebuffer != 0);
    CHECK_EQ(glIsTexture(installed.depthArray), GL_TRUE);
    CHECK_EQ(glIsTexture(installed.transmitArray), GL_TRUE);
    CHECK_EQ(glIsFramebuffer(installed.framebuffer), GL_TRUE);
}

TEST_CASE(ApplicationPreferences_UncertainShadowDisableKeepsPublishedOffState) {
    ShadowFixture fixture;
    auto preferences = fixture.owner(true);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);
    const ShadowResources retired = shadowResources(fixture.view);
    Rigel::Preferences::detail::
        setUserPreferencesAfterPublicationHookForTesting(
            &failAfterPublication);

    const auto result = preferences.applyShadows(fixture.view, false);

    CHECK_EQ(
        result.status,
        Rigel::PreferenceApplyStatus::PublishedDurabilityUncertain);
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK(!persistedShadows(fixture.path));
    const ShadowResources installed = shadowResources(fixture.view);
    CHECK_EQ(installed.depthArray, static_cast<GLuint>(0));
    CHECK_EQ(installed.transmitArray, static_cast<GLuint>(0));
    CHECK_EQ(installed.framebuffer, static_cast<GLuint>(0));
    CHECK_EQ(glIsTexture(retired.depthArray), GL_FALSE);
    CHECK_EQ(glIsTexture(retired.transmitArray), GL_FALSE);
    CHECK_EQ(glIsFramebuffer(retired.framebuffer), GL_FALSE);
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

TEST_CASE(ApplicationPreferences_MissingShadowDepthShaderRejectsLiveEnable) {
    MissingShadowDepthFixture fixture;
    auto preferences = fixture.owner(false);
    CHECK_EQ(
        preferences.initializeShadows(fixture.view).status,
        Rigel::PreferenceApplyStatus::Applied);

    const auto result = preferences.applyShadows(fixture.view, true);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(
        result.message,
        "the voxel shadow depth shader is unavailable");
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(!preferences.requested().graphics.shadows);
    CHECK(!persistedShadows(fixture.path));
    const ShadowResources resources = shadowResources(fixture.view);
    CHECK_EQ(resources.depthArray, static_cast<GLuint>(0));
    CHECK_EQ(resources.transmitArray, static_cast<GLuint>(0));
    CHECK_EQ(resources.framebuffer, static_cast<GLuint>(0));
}

TEST_CASE(ApplicationPreferences_MissingShadowDepthShaderRejectsStartupOn) {
    MissingShadowDepthFixture fixture;
    auto preferences = fixture.owner(true);

    const auto result = preferences.initializeShadows(fixture.view);

    CHECK_EQ(result.status, Rigel::PreferenceApplyStatus::Rejected);
    CHECK_EQ(
        result.message,
        "the voxel shadow depth shader is unavailable");
    CHECK(!preferences.effectiveShadowsEnabled());
    CHECK(preferences.requested().graphics.shadows);
    CHECK(persistedShadows(fixture.path));
    const ShadowResources resources = shadowResources(fixture.view);
    CHECK_EQ(resources.depthArray, static_cast<GLuint>(0));
    CHECK_EQ(resources.transmitArray, static_cast<GLuint>(0));
    CHECK_EQ(resources.framebuffer, static_cast<GLuint>(0));
}
