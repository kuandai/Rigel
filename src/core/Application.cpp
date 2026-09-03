#include "Rigel/Application.h"
#include "ApplicationPreferences.h"
#include "ApplicationEntry.h"
#include "ApplicationTestAccess.h"
#include "DeveloperDiagnostics.h"
#include "GlfwRuntime.h"
#include "StreamingPolicy.h"
#include "WorldGenerationBootstrap.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/InMemoryStorage.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Render/OpenGLRuntime.h"
#include "Rigel/Voxel/ChunkBenchmark.h"
#include "Rigel/Voxel/BlockGalleryCatalog.h"
#include "Rigel/Voxel/BlockGalleryChunkGenerator.h"
#include "Rigel/Voxel/BlockGalleryTargetPresentation.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/GeneratorDefinitionLoader.h"
#include "Rigel/Voxel/WorldSet.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/WorldSpawn.h"
#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/input/GameplayInput.h"
#include <spdlog/spdlog.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/version.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rigel {

namespace {

constexpr float kMaxFrameTime = 0.05f;
constexpr uint32_t kDefaultWorldSeed = 1337;
constexpr std::string_view kDefaultGeneratorDefinitionId = "rigel:default";
constexpr std::string_view kInstalledPersistenceFormat = "cr";
constexpr std::string_view kBlockGalleryPersistenceFormat = "memory";
constexpr std::string_view kBlockGalleryVirtualRoot =
    "developer/block-gallery";
constexpr float kBlockTargetDistance = 8.0f;

void applyInstalledPersistencePolicy(
    Voxel::WorldSet& worldSet,
    Voxel::World& world) {
    worldSet.setPersistencePreferredFormat(
        std::string(kInstalledPersistenceFormat));
    world.persistenceProviders().add(
        Persistence::Backends::CR::kCRSettingsProviderId,
        std::make_shared<
            Persistence::Backends::CR::CRPersistenceSettings>());
}

void applyBlockGalleryPersistencePolicy(Voxel::WorldSet& worldSet) {
    worldSet.setPersistencePreferredFormat(
        std::string(kBlockGalleryPersistenceFormat));
}

void applyBlockGalleryOverview(
    Input::CameraState& camera,
    const Voxel::BlockGalleryOverview& overview) {
    camera.position = {
        overview.centerX + overview.cameraDistance,
        overview.cameraHeight,
        overview.centerZ + overview.cameraDistance,
    };
    const glm::vec3 focus{
        overview.centerX,
        static_cast<float>(Voxel::BlockGalleryCatalog::SpecimenHeight),
        overview.centerZ,
    };
    camera.forward = glm::normalize(focus - camera.position);
    camera.target = focus;
    camera.yaw = glm::degrees(std::atan2(
        camera.forward.z, camera.forward.x));
    camera.pitch = glm::degrees(std::asin(camera.forward.y));
    camera.right = glm::normalize(glm::cross(
        camera.forward, glm::vec3{0.0f, 1.0f, 0.0f}));
    camera.up = glm::normalize(glm::cross(camera.right, camera.forward));
}

void connectChunkLoader(
    Voxel::WorldView& view,
    const std::shared_ptr<Persistence::AsyncChunkLoader>& loader) {
    view.setChunkLoader([loader](Voxel::ChunkLoadRequest request) {
        return loader
            ? loader->request(request)
            : Voxel::ChunkLoadRequestResult::Missing;
    });
    view.setChunkPendingCallback([loader](Voxel::ChunkCoord coord) {
        return loader ? loader->isPending(coord) : false;
    });
    view.setChunkLoadDrain([loader](size_t budget) {
        if (loader) {
            return loader->drainCompletions(budget);
        }
        return std::vector<Voxel::ChunkLoadCompletion>{};
    });
    view.setChunkLoadCancel([loader](Voxel::ChunkCoord coord) {
        if (loader) {
            loader->cancel(coord);
        }
    });
    view.setChunkLoadDiagnosticsCallback([loader]() {
        return loader
            ? loader->diagnostics()
            : Voxel::ChunkLoadDiagnosticSnapshot{};
    });
    view.setChunkLoadExecutionStateCallback(
        [loader](Voxel::ChunkCoord coord) {
            return loader
                ? loader->executionState(coord)
                : std::optional<Voxel::ChunkLoadExecutionState>{};
        });
    view.setChunkEvictionCallback([loader](Voxel::ChunkCoord coord) {
        return loader ? loader->persistChunk(coord) : false;
    });
}

void disconnectChunkLoader(Voxel::WorldView& view) {
    view.setChunkLoader({});
    view.setChunkPendingCallback({});
    view.setChunkLoadDrain({});
    view.setChunkLoadCancel({});
    view.setChunkLoadDiagnosticsCallback({});
    view.setChunkLoadExecutionStateCallback({});
    view.setChunkEvictionCallback({});
}

bool initializeOptionalUserInterface(
    GLFWwindow* window,
    bool (*initialize)(GLFWwindow*)) noexcept {
    try {
        if (initialize(window)) {
            return true;
        }
        UI::shutdown();
        spdlog::warn("Optional startup resource 'ImGui' failed to initialize");
    } catch (const std::exception& error) {
        UI::shutdown();
        spdlog::warn(
            "Optional startup resource 'ImGui' failed to initialize: {}",
            error.what());
    } catch (...) {
        UI::shutdown();
        spdlog::warn(
            "Optional startup resource 'ImGui' failed to initialize: unknown error");
    }
    return false;
}

std::pair<int, int> requireFramebufferSize(const GlfwRuntime& runtime) {
    const auto size = runtime.framebufferSize();
    if (!size) {
        throw std::runtime_error(
            "Failed to query framebuffer size: " + runtime.lastError());
    }
    return *size;
}

struct ViewDistanceBoundaryObserved final {};

struct ViewDistanceBoundaryProbe {
    Application* application = nullptr;
    Voxel::WorldView* view = nullptr;
    ApplicationViewDistanceState* observed = nullptr;
    std::filesystem::path preferencesPath;
    int candidateChunks = 0;
};

ViewDistanceBoundaryProbe* g_viewDistanceBoundaryProbe = nullptr;

struct BlockGalleryLaunchLifecycleProbe {
    ApplicationBlockGalleryLifecycleState* state = nullptr;
    GlfwRuntime::Api runtimeApi;
    std::filesystem::path userPreferencesPath;
    std::shared_ptr<Persistence::InMemoryStorageBackend> storage;
    Voxel::BlockGalleryWorldPosition specimenPosition;
    Voxel::BlockID specimenId = Voxel::BlockRegistry::airId();
    glm::vec3 movementStart{};
    bool specimenSelected = false;
    bool movementStarted = false;
    bool targetingStarted = false;
    bool mutationInputsQueued = false;
};

BlockGalleryLaunchLifecycleProbe* g_blockGalleryLaunchLifecycleProbe = nullptr;

double viewDistanceBoundaryTime() {
    return 0.0;
}

int viewDistanceBoundaryWindowShouldClose(GLFWwindow*) {
    return GLFW_FALSE;
}

void viewDistanceBoundaryPollEvents() {
    auto& probe = *g_viewDistanceBoundaryProbe;
    auto& application = *probe.application;
    auto& view = *probe.view;
    auto& observed = *probe.observed;

    observed.requestResult =
        application.applyViewDistance(probe.candidateChunks);
    observed.beforeRequestedChunks =
        application.requestedPreferences().graphics.viewDistanceChunks;
    observed.beforePersistedChunks =
        Preferences::UserPreferencesStore(probe.preferencesPath)
            .load()
            .graphics.viewDistanceChunks;
    observed.beforeEffectiveChunks = application.effectiveViewDistanceChunks();
    observed.beforeStreamedChunks = view.viewDistanceChunks();
    observed.beforeRenderDistance = view.renderDistanceWorldUnits();
    if (view.viewDistancePolicy()) {
        observed.beforePolicyGeneration =
            view.viewDistancePolicy()->generation();
    }
}

void viewDistanceBoundarySwapBuffers(GLFWwindow*) {
}

void observeViewDistanceBoundary(
    Application& application,
    const std::optional<PreferenceApplyResult>& result
) {
    auto& probe = *g_viewDistanceBoundaryProbe;
    auto& view = *probe.view;
    auto& observed = *probe.observed;

    observed.result = result.value_or(observed.requestResult);
    observed.requestedChunks =
        application.requestedPreferences().graphics.viewDistanceChunks;
    observed.effectiveChunks = application.effectiveViewDistanceChunks();
    observed.streamedChunks = view.viewDistanceChunks();
    observed.renderDistance = view.renderDistanceWorldUnits();
    observed.projectionFarPlane = view.projectionFarPlaneWorldUnits();
    if (view.viewDistancePolicy()) {
        observed.unloadChunks =
            view.viewDistancePolicy()->unloadRadiusChunks();
        observed.preloadRadiusRegions =
            view.viewDistancePolicy()->preloadRadiusRegions();
        observed.shadowDistanceCeiling =
            view.viewDistancePolicy()->shadowDistanceCeilingWorldUnits();
        observed.policyGeneration =
            view.viewDistancePolicy()->generation();
    }
    const auto& work = view.streamingMetrics();
    observed.worldWorkCoordinatesInspected =
        work.lastUpdateDesiredBuildCoordinatesInspected +
        work.lastUpdateSchedulerCoordinatesInspected +
        work.lastUpdateCacheEvictionCoordinatesInspected +
        work.lastUpdateResidentEvictionCoordinatesInspected +
        work.lastUpdateDeferredEvictionCoordinatesInspected;
    throw ViewDistanceBoundaryObserved{};
}

} // namespace

struct Application::Impl {
    struct TimingState {
        double lastTime = 0.0;
        bool benchmarkEnabled = false;
        double benchmarkStartTime = 0.0;
        Voxel::ChunkBenchmarkStats benchmark;
    };

    struct WorldState {
        Voxel::WorldSet worldSet;
        Voxel::WorldId activeWorldId = Voxel::WorldSet::defaultWorldId();
        Voxel::World* world = nullptr;
        Voxel::WorldView* worldView = nullptr;
        std::shared_ptr<Persistence::AsyncChunkLoader> chunkLoader;
        std::unique_ptr<const Voxel::BlockGalleryCatalog> galleryCatalog;
        std::shared_ptr<const Voxel::BlockGalleryChunkGenerator>
            galleryGenerator;
        std::optional<Voxel::BlockTarget> blockTarget;
        std::optional<Voxel::BlockGalleryTargetPresentation>
            galleryTargetPresentation;
        std::optional<Persistence::WorldSettings> settings;
        bool ready = false;
        bool streamingLifecycleLogged = false;
        Voxel::StreamingLifecycleState lastStreamingLifecycle =
            Voxel::StreamingLifecycleState::DiscoveringSpawn;
        Voxel::StreamingDiagnosticSnapshot lastStreamingDiagnostics;
        Voxel::BlockID placeBlock = Voxel::BlockRegistry::airId();
    };

    GlfwRuntime runtime;
    std::unique_ptr<ApplicationPreferences> preferences;
    std::filesystem::path preferencesPath;
    Asset::AssetManager assets;
    Input::WindowState window;
    Input::CameraState camera;
    Input::InputState input;
    std::shared_ptr<const Input::InputBindings> playerDefaultBindings;
    Input::DebugOverlayListener debugOverlayListener;
    Input::ImGuiOverlayListener imguiOverlayListener;
    Render::FrameRenderer renderer;
    TimingState timing;
    WorldState world;
    Input::InputCallbackContext inputCallbacks;
    bool openGLInitialized = false;
    bool shutDown = false;
    bool initializeWindowIntegrations = true;
    WorldMode worldMode = WorldMode::Normal;
    void (*afterContextAcquired)() = nullptr;
    void (*shutdownStageCompleted)(ApplicationShutdownStage) noexcept = nullptr;
    void (*afterDisplayInitialized)(Application&) = nullptr;
    void (*afterInstalledPersistenceContextPrepared)(
        ApplicationPersistencePolicyState) = nullptr;

    Impl() = default;
    explicit Impl(WorldMode mode)
        : worldMode(mode) {}
    explicit Impl(ApplicationConstructionHooks hooks)
        : runtime(hooks.runtimeApi)
        , preferencesPath(std::move(hooks.userPreferencesPath))
        , initializeWindowIntegrations(hooks.initializeWindowIntegrations)
        , worldMode(hooks.worldMode)
        , afterContextAcquired(hooks.afterContextAcquired)
        , shutdownStageCompleted(hooks.shutdownStageCompleted)
        , afterDisplayInitialized(hooks.afterDisplayInitialized)
        , afterInstalledPersistenceContextPrepared(
              hooks.afterInstalledPersistenceContextPrepared) {
    }
    ~Impl();
    void persistWorld();
    void persistPendingResizeForClose();
    void persistPendingResizeForCleanup() noexcept;
    void (*afterViewDistanceFrameBoundary)(
        Application&,
        const std::optional<PreferenceApplyResult>&) = nullptr;
    void close();
    void closeNoThrow() noexcept;
    void shutdown() noexcept;
    void completeShutdownStage(ApplicationShutdownStage stage) noexcept {
        if (shutdownStageCompleted) {
            shutdownStageCompleted(stage);
        }
    }
};

Application::Application()
    : Application(std::make_unique<Impl>()) {
}

Application::Application(LaunchOptions launchOptions)
    : Application([&] {
          if (!g_blockGalleryLaunchLifecycleProbe) {
              return std::make_unique<Impl>(launchOptions.worldMode);
          }
          ApplicationConstructionHooks hooks;
          hooks.runtimeApi =
              g_blockGalleryLaunchLifecycleProbe->runtimeApi;
          hooks.userPreferencesPath =
              g_blockGalleryLaunchLifecycleProbe->userPreferencesPath;
          hooks.worldMode = launchOptions.worldMode;
          hooks.initializeWindowIntegrations = false;
          return std::make_unique<Impl>(std::move(hooks));
      }()) {
}

Application::Application(std::unique_ptr<Impl> impl)
    : Application(std::move(impl), Initialization::Run) {
}

Application::Application(
    std::unique_ptr<Impl> impl,
    Initialization initialization
)
    : m_impl(std::move(impl)) {
    if (initialization == Initialization::Skip) {
        return;
    }
    try {
        initialize();
    } catch (...) {
        m_impl->shutdown();
        throw;
    }
}

void Application::initialize() {
    #ifdef DEBUG
    if (!std::string_view(RIGEL_GIT_HASH).empty()) {
        spdlog::info("Rigel v{} Developer Preview (git {})", RIGEL_VERSION, RIGEL_GIT_HASH);
    } else {
        spdlog::info("Rigel v{} Developer Preview", RIGEL_VERSION);
    }
    #else
    spdlog::info("Rigel v{}", RIGEL_VERSION);
    #endif

#ifdef DEBUG
    std::error_code launchDirectoryError;
    const std::filesystem::path launchDirectory =
        std::filesystem::current_path(launchDirectoryError);
    if (!launchDirectoryError) {
        detail::warnAboutObsoleteConfiguration(
            launchDirectory,
            m_impl->world.activeWorldId);
    }
#endif

    const char* benchEnv = std::getenv("RIGEL_CHUNK_BENCH");
    m_impl->timing.benchmarkEnabled =
        benchEnv && benchEnv[0] != '\0' && benchEnv[0] != '0';

    if (m_impl->preferencesPath.empty()) {
        m_impl->preferencesPath = Preferences::currentUserPreferencesPath();
    }
    m_impl->preferences = std::make_unique<ApplicationPreferences>(
        m_impl->preferencesPath);
    m_impl->preferences->load();

    // Initialize GLFW
    if (!m_impl->runtime.initialize()) {
        spdlog::error("GLFW initialization failed");
        throw std::runtime_error("GLFW initialization failed");
    }
    spdlog::info("GLFW initialized successfully");

    m_impl->runtime.windowHint(
        GLFW_CONTEXT_VERSION_MAJOR, Render::kOpenGLContextMajorVersion);
    m_impl->runtime.windowHint(
        GLFW_CONTEXT_VERSION_MINOR, Render::kOpenGLContextMinorVersion);
    m_impl->runtime.windowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    m_impl->runtime.windowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    m_impl->runtime.windowHint(GLFW_DEPTH_BITS, 24);

    const ApplicationPreferences::StartupResult displayStartup =
        m_impl->preferences->initializeDisplay(
            m_impl->runtime,
            m_impl->timing.benchmarkEnabled);
    m_impl->window.window = m_impl->runtime.window();
    if (displayStartup.usedSafeFallback) {
        spdlog::warn("{}", displayStartup.message);
    }
    if (m_impl->afterContextAcquired) {
        m_impl->afterContextAcquired();
    }
    m_impl->world.worldSet.persistenceFormats().registerFormat(
        Persistence::Backends::CR::descriptor(),
        Persistence::Backends::CR::factory(),
        Persistence::Backends::CR::probe());
    m_impl->world.worldSet.persistenceFormats().registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    const bool blockGallery =
        m_impl->worldMode == WorldMode::BlockGallery;
    if (blockGallery) {
        m_impl->world.worldSet.setPersistenceStorage(
            std::make_shared<Persistence::InMemoryStorageBackend>());
        m_impl->world.worldSet.setPersistenceRoot(
            std::string(kBlockGalleryVirtualRoot));
    } else {
        m_impl->world.worldSet.setPersistenceStorage(
            std::make_shared<Persistence::FilesystemBackend>());
        m_impl->world.worldSet.setPersistenceRoot(
            Persistence::mainWorldRootPath(m_impl->world.activeWorldId));
    }
    m_impl->world.world = &m_impl->world.worldSet.createWorld(
        m_impl->world.activeWorldId);
    if (blockGallery) {
        applyBlockGalleryPersistencePolicy(m_impl->world.worldSet);
    } else {
        applyInstalledPersistencePolicy(
            m_impl->world.worldSet, *m_impl->world.world);
    }
    const Persistence::PersistenceContext bootstrapPersistenceContext =
        m_impl->world.worldSet.persistenceContext(
            m_impl->world.activeWorldId);
    if (m_impl->afterInstalledPersistenceContextPrepared) {
        const auto settings = bootstrapPersistenceContext.providers->findAs<
            Persistence::Backends::CR::CRPersistenceSettings>(
                Persistence::Backends::CR::kCRSettingsProviderId);
        m_impl->afterInstalledPersistenceContextPrepared({
            bootstrapPersistenceContext.preferredFormat,
            bootstrapPersistenceContext.rootPath,
            settings != nullptr,
            settings && settings->enableLz4,
            dynamic_cast<Persistence::InMemoryStorageBackend*>(
                bootstrapPersistenceContext.storage.get()) != nullptr,
        });
    }
    if (m_impl->afterDisplayInitialized) {
        m_impl->afterDisplayInitialized(*this);
    }
    const auto& requestedDisplay =
        m_impl->preferences->requested().display;
    const auto& effectiveDisplay =
        m_impl->preferences->effectiveDisplay();
    spdlog::info(
        "Display request mode={} size={}x{} VSync={} FPS={}; effective mode={} size={}x{} VSync={} FPS={}",
        requestedDisplay.mode == Preferences::DisplayMode::Windowed
            ? "windowed"
            : "borderless",
        requestedDisplay.windowedSize.width,
        requestedDisplay.windowedSize.height,
        requestedDisplay.vsync,
        requestedDisplay.fpsLimit
            ? std::to_string(*requestedDisplay.fpsLimit)
            : "unlimited",
        effectiveDisplay.mode == Preferences::DisplayMode::Windowed
            ? "windowed"
            : "borderless",
        effectiveDisplay.windowedSize.width,
        effectiveDisplay.windowedSize.height,
        effectiveDisplay.vsync,
        effectiveDisplay.fpsLimit
            ? std::to_string(*effectiveDisplay.fpsLimit)
            : "unlimited");

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    bool glewInitialized = glewStatus == GLEW_OK;
#if defined(GLEW_ERROR_NO_GLX_DISPLAY)
    glewInitialized = glewInitialized ||
        (glewStatus == GLEW_ERROR_NO_GLX_DISPLAY &&
         glGetString(GL_VERSION) != nullptr);
#endif
    if (!glewInitialized) {
        spdlog::error("GLEW initialization failed");
        throw std::runtime_error("GLEW initialization failed");
    }
    m_impl->openGLInitialized = true;
    spdlog::info("GLEW initialized successfully");

    // Print OpenGL version
    spdlog::info("OpenGL Version: {}", (char*)glGetString(GL_VERSION));

#if defined(RIGEL_ENABLE_IMGUI)
    if (m_impl->initializeWindowIntegrations) {
        initializeOptionalUserInterface(m_impl->window.window, &UI::init);
    }
#endif

    m_impl->renderer.setVerticalFovDegrees(
        m_impl->preferences->effectiveVerticalFovDegrees());

    const auto [framebufferWidth, framebufferHeight] =
        requireFramebufferSize(m_impl->runtime);
    glViewport(0, 0, framebufferWidth, framebufferHeight);

    // Set Callbacks
    m_impl->runtime.setFramebufferSizeCallback([](
        GLFWwindow*, int width, int height) {
        glViewport(0, 0, width, height);
    });
    m_impl->inputCallbacks.input = &m_impl->input;
    m_impl->inputCallbacks.window = &m_impl->window;
    m_impl->inputCallbacks.camera = &m_impl->camera;
    m_impl->inputCallbacks.effectiveInputPreferences =
        &m_impl->preferences->effectiveInput();
    registerApplicationPreferenceCallbacks(
        m_impl->inputCallbacks, *m_impl->preferences);
    if (m_impl->initializeWindowIntegrations) {
        Input::registerWindowCallbacks(
            m_impl->window.window, m_impl->inputCallbacks);
    }
    m_impl->runtime.setWindowSizeCallback([](
        GLFWwindow* window, int width, int height) {
        auto* context = static_cast<Input::InputCallbackContext*>(
            glfwGetWindowUserPointer(window));
        if (context && context->logicalResize) {
            context->logicalResize(
                context->logicalResizeContext, width, height);
        }
    });
    if (m_impl->initializeWindowIntegrations) {
        Input::setCursorCaptured(m_impl->window, true);
    }
    if (m_impl->timing.benchmarkEnabled) {
        spdlog::info("Chunk benchmark enabled");
    }

    try {
        m_impl->assets.loadManifest("manifest.yaml");
        m_impl->assets.registerLoader("input", std::make_unique<Input::InputBindingsLoader>());
        m_impl->assets.registerLoader("entity_models", std::make_unique<Entity::EntityModelLoader>());
        m_impl->assets.registerLoader("entity_anims", std::make_unique<Entity::EntityAnimationSetLoader>());
        detail::StreamingPolicy streamingPolicy =
            detail::makeAutomaticStreamingPolicy();
        const Voxel::StreamingConfig& streamingConfig =
            streamingPolicy.streamer;
        m_impl->world.worldSet.initializeResources(m_impl->assets);

        if (blockGallery) {
            auto galleryCatalog =
                std::make_unique<const Voxel::BlockGalleryCatalog>(
                    m_impl->world.worldSet.resources().registry());
            m_impl->world.galleryGenerator =
                std::make_shared<const Voxel::BlockGalleryChunkGenerator>(
                    m_impl->world.worldSet.resources().registry(),
                    *galleryCatalog);
            m_impl->world.galleryCatalog = std::move(galleryCatalog);
        }

        m_impl->playerDefaultBindings =
            Input::loadPlayerDefaultBindings(m_impl->assets);
        m_impl->preferences->initializeInput(
            m_impl->input,
            *m_impl->playerDefaultBindings);
        m_impl->debugOverlayListener.enabled = &m_impl->renderer.debugOverlayEnabled();
        m_impl->input.addListener(&m_impl->debugOverlayListener);
        m_impl->imguiOverlayListener.enabled = &m_impl->renderer.profilerWindowEnabled();
        m_impl->input.addListener(&m_impl->imguiOverlayListener);

        m_impl->world.worldView = &m_impl->world.worldSet.createView(m_impl->world.activeWorldId, m_impl->assets);
        Persistence::NewWorldGenerationFactory creationFactory = [&] {
            if (m_impl->world.galleryGenerator) {
                return Persistence::NewWorldGeneration{
                    "Block gallery",
                    0,
                    Voxel::prepareBlockGalleryGeneratorIdentity(
                        m_impl->world.worldSet.resources().registry(),
                        m_impl->world.galleryGenerator->worldBounds())};
            }
            return Persistence::NewWorldGeneration{
                "world_" + std::to_string(m_impl->world.activeWorldId),
                kDefaultWorldSeed,
                Voxel::loadPreparedGeneratorDefinitionSnapshot(
                    m_impl->assets,
                    m_impl->world.worldSet.resources().registry(),
                    kDefaultGeneratorDefinitionId)};
        };

        detail::ApplicationWorldGenerationBootstrapResult bootstrapped =
            detail::bootstrapApplicationWorldGeneration(
                m_impl->world.worldSet,
                m_impl->world.activeWorldId,
                *m_impl->world.world,
                *m_impl->world.worldView,
                creationFactory,
                bootstrapPersistenceContext,
                m_impl->world.galleryGenerator);
        auto generator = std::move(bootstrapped.generator);
        m_impl->world.settings = std::move(bootstrapped.settings);
        Persistence::PersistenceContext persistenceContext =
            bootstrapPersistenceContext;
        persistenceContext.preferredFormat =
            bootstrapped.persistenceFormat;
        persistenceContext.discoverExistingFormat = false;

        Persistence::loadBootstrapEntities(
            *m_impl->world.world,
            m_impl->assets,
            m_impl->world.worldSet.persistenceService(),
            persistenceContext);

        uint32_t worldGenVersion = generator->semanticsVersion();
        m_impl->world.chunkLoader = std::make_shared<Persistence::AsyncChunkLoader>(
            m_impl->world.worldSet.persistenceService(),
            std::move(persistenceContext),
            *m_impl->world.world,
            worldGenVersion,
            streamingPolicy.ioThreads,
            streamingPolicy.loadWorkerThreads,
            generator);
        m_impl->world.chunkLoader->setLoadQueueLimit(
            streamingPolicy.loadQueueLimit);
        m_impl->world.chunkLoader->setRegionDrainBudget(
            streamingPolicy.loadRegionDrainBudget);
        m_impl->world.chunkLoader->setMaxCachedRegions(
            streamingPolicy.loadMaxCachedRegions);
        m_impl->world.chunkLoader->setMaxInFlightRegions(
            streamingPolicy.loadMaxInFlightRegions);
        connectChunkLoader(
            *m_impl->world.worldView, m_impl->world.chunkLoader);

        Core::Profiler::setEnabled(
            detail::profilerEnabledFromEnvironment());
        m_impl->world.worldView->setStreamConfig(streamingConfig);
        m_impl->preferences->initializeViewDistance(
            *m_impl->world.worldView,
            m_impl->world.chunkLoader.get());
        const PreferenceApplyResult shadowStartup =
            m_impl->preferences->initializeShadows(
                *m_impl->world.worldView);
        if (shadowStartup.status == PreferenceApplyStatus::Rejected) {
            spdlog::warn(
                "Requested shadows could not be enabled at startup; "
                "continuing with shadows off: {}",
                shadowStartup.message);
        }
        spdlog::info(
            "Shadows request={}; effective={}",
            m_impl->preferences->requested().graphics.shadows
                ? "on"
                : "off",
            m_impl->preferences->effectiveShadowsEnabled()
                ? "on"
                : "off");
        if (m_impl->timing.benchmarkEnabled) {
            m_impl->world.worldView->setBenchmark(&m_impl->timing.benchmark);
        }

        auto placeId = m_impl->world.world->blockRegistry().findByIdentifier(
            generator->definition().terrain.solidMaterial);
        if (!placeId) {
            placeId = m_impl->world.world->blockRegistry().findByIdentifier("base:stone_shale");
        }
        if (placeId) {
            m_impl->world.placeBlock = *placeId;
        } else if (m_impl->world.world->blockRegistry().size() > 1) {
            m_impl->world.placeBlock = Voxel::BlockID{1};
        }

        if (m_impl->world.galleryGenerator) {
            applyBlockGalleryOverview(
                m_impl->camera,
                m_impl->world.galleryGenerator->overview());
        } else {
            int spawnX = static_cast<int>(
                std::floor(m_impl->camera.position.x));
            int spawnZ = static_cast<int>(
                std::floor(m_impl->camera.position.z));
            int spawnY = Voxel::findFirstAirY(
                *generator, spawnX, spawnZ);
            m_impl->camera.position.y = static_cast<float>(spawnY) + 0.5f;
        }
        m_impl->world.worldView->markSpawnDiscoveryComplete();

        m_impl->renderer.initialize(m_impl->assets);
        m_impl->world.ready = true;
        ApplicationTestAccess::observeBlockGalleryLaunchInitialized(*this);
    } catch (const std::exception& e) {
        spdlog::error("Voxel bootstrap failed: {}", e.what());
        throw;
    }
}

Application::Impl::~Impl() {
    shutdown();
}

void Application::Impl::persistWorld() {
    if (!world.ready || !world.world) {
        return;
    }
    if (!world.settings) {
        throw std::logic_error(
            "Cannot persist a world without save-owned settings");
    }

    Persistence::saveWorldToDisk(
        *world.world,
        *world.settings,
        world.worldSet.persistenceService(),
        world.worldSet.persistenceContext(world.activeWorldId));
}

void Application::Impl::persistPendingResizeForClose() {
    if (!preferences) {
        return;
    }
    const auto result = preferences->flushResizePersistenceForShutdown();
    if (!result) {
        return;
    }
    if (result->status == PreferenceApplyStatus::NotPublished) {
        throw std::runtime_error(
            "Failed to save pending window resize during application close: " +
            result->message);
    }
    if (result->status == PreferenceApplyStatus::PersistenceBlocked) {
        return;
    }
    if (result->status ==
        PreferenceApplyStatus::PublishedDurabilityUncertain) {
        spdlog::warn(
            "Pending window resize was published during application close, "
            "but durability is uncertain: {}",
            result->message);
    }
}

void Application::Impl::persistPendingResizeForCleanup() noexcept {
    if (!preferences) {
        return;
    }
    try {
        const auto result = preferences->flushResizePersistenceForShutdown();
        if (!result) {
            return;
        }
        if (result->status == PreferenceApplyStatus::NotPublished) {
            spdlog::error(
                "Pending window resize was not saved during application "
                "cleanup: {}",
                result->message);
        } else if (
            result->status == PreferenceApplyStatus::PersistenceBlocked) {
            spdlog::error(
                "Pending window resize cannot be saved because preference "
                "persistence is blocked: {}",
                result->message);
        } else if (result->status ==
                   PreferenceApplyStatus::PublishedDurabilityUncertain) {
            spdlog::warn(
                "Pending window resize was published during application "
                "cleanup, but durability is uncertain: {}",
                result->message);
        }
    } catch (const std::exception& error) {
        spdlog::error(
            "Pending window resize save failed during application cleanup: {}",
            error.what());
    } catch (...) {
        spdlog::error(
            "Pending window resize save failed during application cleanup: "
            "unknown failure");
    }
}

std::optional<PreferenceApplyResult>
Application::consumePendingViewDistanceAtFrameBoundary() {
    if (!m_impl || !m_impl->preferences) {
        return std::nullopt;
    }
    if (!m_impl->world.ready || !m_impl->world.worldView) {
        m_impl->preferences->discardPendingViewDistance();
        return std::nullopt;
    }
    return m_impl->preferences->consumePendingViewDistance(
        *m_impl->world.worldView,
        m_impl->world.chunkLoader.get());
}

void Application::Impl::close() {
    if (shutDown) {
        return;
    }

    persistPendingResizeForClose();

    try {
        persistWorld();
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("Failed to save world during application close: ") +
            e.what());
    } catch (...) {
        throw std::runtime_error(
            "Failed to save world during application close: unknown failure");
    }

    shutdown();
}

void Application::Impl::closeNoThrow() noexcept {
    if (shutDown) {
        return;
    }

    try {
        persistWorld();
    } catch (const std::exception& e) {
        spdlog::error("World save failed during application cleanup: {}", e.what());
    } catch (...) {
        spdlog::error("World save failed during application cleanup: unknown failure");
    }

    shutdown();
}

void Application::Impl::shutdown() noexcept {
    if (std::exchange(shutDown, true)) {
        return;
    }

    if (preferences) {
        preferences->discardPendingViewDistance();
    }

    persistPendingResizeForCleanup();

    const bool hasContext = runtime.window() != nullptr;
    if (hasContext) {
        runtime.makeContextCurrent();
        completeShutdownStage(ApplicationShutdownStage::ContextMadeCurrent);
        UI::shutdown();
    }
    completeShutdownStage(ApplicationShutdownStage::UserInterfaceReleased);

    Voxel::WorldView* activeView = world.worldView;
    if (!activeView) {
        activeView = world.worldSet.findView(world.activeWorldId);
    }
    if (activeView) {
        disconnectChunkLoader(*activeView);
    }
    world.chunkLoader.reset();
    completeShutdownStage(ApplicationShutdownStage::AsyncLoadingStopped);

    if (activeView) {
        activeView->clear();
        if (openGLInitialized && hasContext) {
            activeView->releaseRenderResources();
        }
    }
    world.blockTarget.reset();
    world.galleryTargetPresentation.reset();
    world.galleryGenerator.reset();
    world.galleryCatalog.reset();
    world.worldSet.clear();
    world.worldView = nullptr;
    world.world = nullptr;
    world.ready = false;
    completeShutdownStage(ApplicationShutdownStage::WorldsReleased);

    if (openGLInitialized && hasContext) {
        world.worldSet.resources().releaseRenderResources();
        renderer.release();
    }
    completeShutdownStage(ApplicationShutdownStage::RenderResourcesReleased);
    assets.clearCache();
    completeShutdownStage(ApplicationShutdownStage::AssetCacheReleased);

    window.window = nullptr;
    runtime.shutdown();
    completeShutdownStage(ApplicationShutdownStage::RuntimeReleased);
    openGLInitialized = false;
}

Application::~Application() {
    if (m_impl) {
        m_impl->closeNoThrow();
    }
}

void Application::close() {
    if (m_impl) {
        m_impl->close();
    }
}

void ApplicationTestAccess::construct(ApplicationConstructionHooks hooks) {
    if (hooks.userPreferencesPath.empty()) {
        hooks.userPreferencesPath = std::filesystem::absolute(
            "application-test-user-preferences.yaml");
    }
    Application application(std::make_unique<Application::Impl>(hooks));
}

void ApplicationTestAccess::constructAndRun(
    ApplicationConstructionHooks hooks,
    void (*runLoop)(Application&)
) {
    if (hooks.userPreferencesPath.empty()) {
        hooks.userPreferencesPath = std::filesystem::absolute(
            "application-test-user-preferences.yaml");
    }
    Application application(std::make_unique<Application::Impl>(hooks));
    runLoop(application);
}

void ApplicationTestAccess::closeReadyWorld(ApplicationCloseHooks hooks) {
    auto impl = std::make_unique<Application::Impl>();
    impl->shutdownStageCompleted = hooks.shutdownStageCompleted;
    impl->world.worldSet.persistenceFormats().registerFormat(
        Persistence::Backends::Memory::descriptor(),
        Persistence::Backends::Memory::factory(),
        Persistence::Backends::Memory::probe());
    impl->world.worldSet.setPersistenceStorage(
        std::move(hooks.persistenceStorage));
    impl->world.worldSet.setPersistenceRoot(std::move(hooks.persistenceRoot));
    impl->world.worldSet.setPersistencePreferredFormat("memory");

    Voxel::World& world = impl->world.worldSet.createWorld(
        impl->world.activeWorldId);
    Voxel::Chunk& dirtyChunk = world.chunkManager().getOrCreateChunk({0, 0, 0});
    dirtyChunk.markPersistDirty();
    auto entity = std::make_unique<Entity::Entity>("rigel:close_test_entity");
    entity->setPosition(1.0f, 2.0f, 3.0f);
    world.entities().spawn(std::move(entity));
    impl->world.world = &world;
    impl->world.settings = Persistence::loadSavedWorldGeneration(
        impl->world.worldSet.persistenceContext(world.id())).settings;
    impl->world.ready = true;

    Application application(
        std::move(impl), Application::Initialization::Skip);
    try {
        application.close();
    } catch (...) {
        if (hooks.closeFailureObserved) {
            hooks.closeFailureObserved(dirtyChunk.isPersistDirty());
        }
        throw;
    }
}

void ApplicationTestAccess::closeWithPendingResize(
    std::filesystem::path userPreferencesPath,
    int width,
    int height) {
    auto impl = std::make_unique<Application::Impl>();
    impl->preferences = std::make_unique<ApplicationPreferences>(
        std::move(userPreferencesPath));
    impl->preferences->load();
    impl->preferences->acceptLogicalResize({width, height}, 0.0);
    Application application(
        std::move(impl), Application::Initialization::Skip);
    application.close();
}

void ApplicationTestAccess::shutdownWithPendingResize(
    std::filesystem::path userPreferencesPath,
    int width,
    int height) {
    auto impl = std::make_unique<Application::Impl>();
    impl->preferences = std::make_unique<ApplicationPreferences>(
        std::move(userPreferencesPath));
    impl->preferences->load();
    impl->preferences->acceptLogicalResize({width, height}, 0.0);
    impl->shutdown();
}

ApplicationViewDistanceState
ApplicationTestAccess::applyViewDistanceAtFrameBoundary(
    std::filesystem::path userPreferencesPath,
    int initialChunks,
    int candidateChunks,
    bool activeSession) {
    Preferences::UserPreferences requested;
    requested.graphics.viewDistanceChunks = initialChunks;
    if (!std::filesystem::exists(userPreferencesPath)) {
        Preferences::UserPreferencesStore(userPreferencesPath)
            .saveRequested(requested);
    }

    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    Voxel::StreamingConfig streaming;
    streaming.viewDistanceChunks = 3;
    streaming.unloadDistanceChunks = 20;
    streaming.workerThreads = 0;
    view.setStreamConfig(streaming);

    GlfwRuntime::Api runtimeApi{};
    runtimeApi.getTime = &viewDistanceBoundaryTime;
    runtimeApi.windowShouldClose =
        &viewDistanceBoundaryWindowShouldClose;
    runtimeApi.pollEvents = &viewDistanceBoundaryPollEvents;
    runtimeApi.swapBuffers = &viewDistanceBoundarySwapBuffers;
    ApplicationConstructionHooks construction;
    construction.runtimeApi = runtimeApi;
    auto impl = std::make_unique<Application::Impl>(construction);
    const auto persistedPreferencesPath = userPreferencesPath;
    impl->preferences = std::make_unique<ApplicationPreferences>(
        std::move(userPreferencesPath));
    impl->preferences->load();
    impl->preferences->initializeViewDistance(view);
    impl->window.window = reinterpret_cast<GLFWwindow*>(0x1);
    impl->world.world = &world;
    impl->world.worldView = &view;
    impl->world.ready = activeSession;
    impl->afterViewDistanceFrameBoundary = &observeViewDistanceBoundary;
    Application::Impl* state = impl.get();

    Application application(
        std::move(impl), Application::Initialization::Skip);
    ApplicationViewDistanceState observed;
    ViewDistanceBoundaryProbe probe{
        &application,
        &view,
        &observed,
        persistedPreferencesPath,
        candidateChunks,
    };
    g_viewDistanceBoundaryProbe = &probe;
    try {
        application.run();
    } catch (const ViewDistanceBoundaryObserved&) {
        g_viewDistanceBoundaryProbe = nullptr;
    } catch (...) {
        g_viewDistanceBoundaryProbe = nullptr;
        throw;
    }

    state->world.ready = false;
    state->world.worldView = nullptr;
    state->world.world = nullptr;
    state->window.window = nullptr;
    return observed;
}

ApplicationBlockGalleryLifecycleState
ApplicationTestAccess::runBlockGalleryLaunchLifecycle(
    int argc,
    const char* const* argv,
    GlfwRuntime::Api runtimeApi,
    std::filesystem::path userPreferencesPath
) {
    if (g_blockGalleryLaunchLifecycleProbe) {
        throw std::logic_error(
            "An application launch lifecycle exercise is already active");
    }

    ApplicationBlockGalleryLifecycleState observed;
    BlockGalleryLaunchLifecycleProbe probe;
    probe.state = &observed;
    probe.runtimeApi = runtimeApi;
    probe.userPreferencesPath = std::move(userPreferencesPath);
    g_blockGalleryLaunchLifecycleProbe = &probe;
    observed.exitCode = runApplication(argc, argv);
    g_blockGalleryLaunchLifecycleProbe = nullptr;
    observed.generatedChunkPersistedOnClose =
        probe.storage && probe.storage->exists(
            std::string(kBlockGalleryVirtualRoot) +
            "/zones/rigel/default/regions");
    return observed;
}

void ApplicationTestAccess::observeBlockGalleryLaunchInitialized(
    Application& application
) {
    if (!g_blockGalleryLaunchLifecycleProbe) {
        return;
    }

    BlockGalleryLaunchLifecycleProbe& probe =
        *g_blockGalleryLaunchLifecycleProbe;
    ApplicationBlockGalleryLifecycleState& observed = *probe.state;
    Application::Impl& impl = *application.m_impl;
    observed.decodedWorldMode = impl.worldMode;
    if (impl.worldMode != WorldMode::BlockGallery ||
        !impl.world.world || !impl.world.worldView ||
        !impl.world.galleryCatalog || !impl.world.galleryGenerator) {
        return;
    }

    const Persistence::PersistenceContext context =
        impl.world.worldSet.persistenceContext(impl.world.activeWorldId);
    observed.persistenceRoot = context.rootPath;
    probe.storage = std::dynamic_pointer_cast<
        Persistence::InMemoryStorageBackend>(context.storage);
    observed.processPrivateStorage = probe.storage != nullptr;

    Voxel::WorldResources& resources = impl.world.worldSet.resources();
    const Voxel::BlockRegistry& registry = resources.registry();
    const Voxel::BlockGalleryCatalog& catalog = *impl.world.galleryCatalog;
    observed.resourcesInitialized = resources.initialized();
    observed.runtimeRegistrationCount = registry.size();
    observed.gallerySpecimenCount = catalog.entries().size();
    observed.emptyGeometryExclusionCount =
        catalog.emptyGeometryExclusions().size();
    observed.textureCount = resources.textureAtlas().textureCount();
    observed.worldBootstrapped =
        impl.world.world->generator() &&
        impl.world.world->generator() == impl.world.worldView->generator() &&
        impl.world.chunkLoader != nullptr;

    const Voxel::BlockGalleryOverview overview =
        impl.world.galleryGenerator->overview();
    const glm::vec3 expectedPosition{
        overview.centerX + overview.cameraDistance,
        overview.cameraHeight,
        overview.centerZ + overview.cameraDistance,
    };
    const glm::vec3 expectedTarget{
        overview.centerX,
        static_cast<float>(Voxel::BlockGalleryCatalog::SpecimenHeight),
        overview.centerZ,
    };
    observed.overviewInstalled =
        glm::distance(impl.camera.position, expectedPosition) < 0.0001f &&
        glm::distance(impl.camera.target, expectedTarget) < 0.0001f;

    float closestDistanceSquared = std::numeric_limits<float>::max();
    for (const Voxel::BlockGalleryCatalogEntry& entry : catalog.entries()) {
        const float dx = static_cast<float>(entry.specimenPosition.x) -
            impl.camera.position.x;
        const float dz = static_cast<float>(entry.specimenPosition.z) -
            impl.camera.position.z;
        const float distanceSquared = dx * dx + dz * dz;
        if (distanceSquared < closestDistanceSquared) {
            closestDistanceSquared = distanceSquared;
            probe.specimenPosition = entry.specimenPosition;
            probe.specimenId = entry.blockId;
            probe.specimenSelected = true;
        }
    }
}

void ApplicationTestAccess::observeBlockGalleryLaunchFrame(
    Application& application
) {
    if (!g_blockGalleryLaunchLifecycleProbe) {
        return;
    }

    BlockGalleryLaunchLifecycleProbe& probe =
        *g_blockGalleryLaunchLifecycleProbe;
    ApplicationBlockGalleryLifecycleState& observed = *probe.state;
    Application::Impl& impl = *application.m_impl;
    if (impl.worldMode != WorldMode::BlockGallery ||
        !impl.world.world || !impl.world.worldView ||
        !impl.world.chunkLoader) {
        impl.runtime.requestWindowClose();
        return;
    }

    ++observed.renderedFrames;
    observed.frameRendererSubmitted = true;
    observed.chunkLoadsStarted =
        impl.world.chunkLoader->diagnostics().work.started;

    if (!probe.movementStarted) {
        probe.movementStart = impl.camera.position;
        impl.input.handleKeyEvent(GLFW_KEY_W, GLFW_PRESS);
        probe.movementStarted = true;
    } else if (!observed.freeFlyMoved &&
               glm::distance(impl.camera.position, probe.movementStart) >
                   0.0001f) {
        observed.freeFlyMoved = true;
        impl.input.handleKeyEvent(GLFW_KEY_W, GLFW_RELEASE);
    }

    if (probe.specimenSelected) {
        const auto& position = probe.specimenPosition;
        const Voxel::ChunkCoord chunk = Voxel::worldToChunk(
            position.x, position.y, position.z);
        observed.specimenLoadedThroughAsyncLoader =
            impl.world.world->getBlock(
                position.x, position.y, position.z).id == probe.specimenId;
        const auto mesh = impl.world.worldView->meshStore().snapshot(chunk);
        observed.specimenMeshSubmitted =
            observed.specimenLoadedThroughAsyncLoader &&
            mesh.has_value() && !mesh->empty;

        if (observed.freeFlyMoved &&
            observed.specimenLoadedThroughAsyncLoader &&
            !probe.targetingStarted) {
            impl.camera.position = {
                static_cast<float>(position.x) + 0.5f,
                static_cast<float>(position.y) + 0.5f,
                static_cast<float>(position.z) + 2.5f,
            };
            impl.camera.yaw = -90.0f;
            impl.camera.pitch = 0.0f;
            impl.camera.forward = {0.0f, 0.0f, -1.0f};
            impl.camera.target = impl.camera.position + impl.camera.forward;
            probe.targetingStarted = true;
        }

        if (probe.targetingStarted &&
            impl.world.galleryTargetPresentation) {
            const auto& presentation =
                *impl.world.galleryTargetPresentation;
            observed.specimenTargetPresented =
                presentation.blockStateIdentifier ==
                    impl.world.world->blockRegistry()
                        .getType(probe.specimenId).identifier;
        }

        if (observed.specimenTargetPresented &&
            !probe.mutationInputsQueued) {
            impl.input.handleMouseButtonEvent(
                GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
            impl.input.handleMouseButtonEvent(
                GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
            impl.input.handleKeyEvent(GLFW_KEY_F2, GLFW_PRESS);
            probe.mutationInputsQueued = true;
        } else if (probe.mutationInputsQueued &&
                   !observed.galleryMutationsSuppressed) {
            observed.galleryMutationsSuppressed =
                impl.world.world->getBlock(
                    position.x, position.y, position.z).id ==
                    probe.specimenId &&
                impl.world.world->getBlock(
                    position.x, position.y, position.z + 1).isAir() &&
                impl.world.world->entities().size() == 0;
            impl.input.handleMouseButtonEvent(
                GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
            impl.input.handleMouseButtonEvent(
                GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
            impl.input.handleKeyEvent(GLFW_KEY_F2, GLFW_RELEASE);
        }
    }

    const bool complete =
        observed.renderedFrames >= 2 &&
        observed.freeFlyMoved &&
        observed.specimenLoadedThroughAsyncLoader &&
        observed.specimenMeshSubmitted &&
        observed.specimenTargetPresented &&
        observed.galleryMutationsSuppressed &&
        observed.chunkLoadsStarted > 0;
    if (complete || observed.renderedFrames >= 600) {
        impl.runtime.requestWindowClose();
    }
}

std::optional<PreferenceApplyResult>
ApplicationTestAccess::consumeViewDistanceOwnerForTesting(
    ApplicationPreferences& preferences,
    Voxel::WorldView& view,
    Persistence::AsyncChunkLoader* loader
) {
    return preferences.consumePendingViewDistance(view, loader);
}

bool ApplicationTestAccess::initializeOptionalUserInterface(
    GLFWwindow* window,
    bool (*initialize)(GLFWwindow*)) noexcept {
    return Rigel::initializeOptionalUserInterface(window, initialize);
}

int runApplication(ApplicationMain applicationMain) noexcept {
    try {
        applicationMain();
        spdlog::info("Application terminated successfully");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        spdlog::error("Application error: {}", e.what());
    } catch (...) {
        spdlog::error("Application error: unknown failure");
    }
    return EXIT_FAILURE;
}

int runApplication(
    int argc,
    const char* const* argv,
    LaunchApplicationMain applicationMain
) noexcept {
    try {
        const LaunchOptions launchOptions = decodeLaunchOptions(argc, argv);
        applicationMain(launchOptions);
        spdlog::info("Application terminated successfully");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        spdlog::error("Application error: {}", e.what());
    } catch (...) {
        spdlog::error("Application error: unknown failure");
    }
    return EXIT_FAILURE;
}

int runApplication(int argc, const char* const* argv) noexcept {
    return runApplication(argc, argv, [](const LaunchOptions& launchOptions) {
        Application application(launchOptions);
        application.run();
        application.close();
    });
}

int runApplication() noexcept {
    const char* argv[] = {"Rigel"};
    return runApplication(1, argv);
}

void Application::run() {
    m_impl->timing.lastTime = m_impl->runtime.time();
    if (m_impl->timing.benchmarkEnabled) {
        m_impl->timing.benchmarkStartTime = m_impl->timing.lastTime;
    }

    // Render loop
    while (!m_impl->runtime.windowShouldClose(m_impl->window.window)) {
        double now = m_impl->runtime.time();
        float deltaTime = static_cast<float>(now - m_impl->timing.lastTime);
        m_impl->timing.lastTime = now;

        // Flush event queue
        m_impl->runtime.pollEvents();
        if (auto resizeObservation =
                m_impl->preferences->consumeLogicalResize(
                    m_impl->runtime,
                    m_impl->preferences->now())) {
            spdlog::error(
                "Window resize could not be observed: {}",
                resizeObservation->message);
        }
        if (auto resizeResult =
                m_impl->preferences->flushResizePersistence(
                    m_impl->preferences->now())) {
            if (resizeResult->status == PreferenceApplyStatus::NotPublished) {
                spdlog::error(
                    "Window resize remains effective but was not saved: {}",
                    resizeResult->message);
            } else if (
                resizeResult->status ==
                PreferenceApplyStatus::PersistenceBlocked) {
                spdlog::error(
                    "Window resize remains effective but cannot be saved; "
                    "automatic retries are disabled: {}",
                    resizeResult->message);
            } else if (
                resizeResult->status ==
                PreferenceApplyStatus::PublishedDurabilityUncertain) {
                spdlog::warn(
                    "Window resize was published but durability is uncertain: {}",
                    resizeResult->message);
            }
        }
        // Runtime preference swaps occur only here, after events have been
        // collected for this application frame and before world work begins.
        const auto viewDistanceResult =
            consumePendingViewDistanceAtFrameBoundary();
        if (m_impl->afterViewDistanceFrameBoundary) {
            m_impl->afterViewDistanceFrameBoundary(*this, viewDistanceResult);
        }
        if (viewDistanceResult) {
            if (viewDistanceResult->status ==
                PreferenceApplyStatus::NotPublished) {
                spdlog::error(
                    "View Distance was not applied because its preference "
                    "could not be published: {}",
                    viewDistanceResult->message);
            } else if (
                viewDistanceResult->status ==
                PreferenceApplyStatus::PersistenceBlocked) {
                spdlog::error(
                    "View Distance was not applied because preference "
                    "persistence is blocked: {}",
                    viewDistanceResult->message);
            } else if (
                viewDistanceResult->status ==
                PreferenceApplyStatus::PublishedDurabilityUncertain) {
                spdlog::warn(
                    "View Distance was applied and published, but durability "
                    "is uncertain: {}",
                    viewDistanceResult->message);
            } else if (
                viewDistanceResult->status ==
                PreferenceApplyStatus::Rejected) {
                spdlog::error(
                    "View Distance policy preparation failed: {}",
                    viewDistanceResult->message);
            }
        }
        if (m_impl->window.pendingTimeReset) {
            m_impl->timing.lastTime = m_impl->runtime.time();
            deltaTime = 0.0f;
            m_impl->window.pendingTimeReset = false;
            m_impl->preferences->resetFramePacingSchedule();
        }
        if (deltaTime > kMaxFrameTime) {
            deltaTime = kMaxFrameTime;
        }
        UI::beginFrame();
        Core::Profiler::beginFrame();
        {
            PROFILE_SCOPE("Frame");
            {
                PROFILE_SCOPE("Input");
                m_impl->renderer.recordFrameTime(deltaTime);
                m_impl->input.beginFrame();
            }

            if (m_impl->world.ready && m_impl->world.world && m_impl->world.worldView) {
                if (m_impl->input.isActionJustPressed("toggle_mouse_capture")) {
                    Input::setCursorCaptured(m_impl->window, !m_impl->window.cursorCaptured);
                }
                if (m_impl->initializeWindowIntegrations &&
                    m_impl->window.cursorCaptured &&
                    glfwGetInputMode(m_impl->window.window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED) {
                    Input::setCursorCaptured(m_impl->window, true);
                }

                {
                    PROFILE_SCOPE("Simulation");
                    Input::updateCamera(m_impl->input, m_impl->camera, deltaTime);
                    const Input::GameplayMutationMode mutationMode =
                        m_impl->worldMode == WorldMode::BlockGallery
                            ? Input::GameplayMutationMode::ReadOnly
                            : Input::GameplayMutationMode::ReadWrite;
                    Input::handleDemoSpawn(
                        m_impl->input,
                        m_impl->assets,
                        *m_impl->world.world,
                        m_impl->camera,
                        mutationMode);
                    const auto refreshBlockTarget = [&] {
                        m_impl->world.blockTarget = Voxel::raycastBlock(
                            *m_impl->world.world,
                            m_impl->camera.position,
                            m_impl->camera.forward,
                            kBlockTargetDistance);
                    };
                    refreshBlockTarget();
                    const bool worldEdited = Input::handleBlockEdits(
                        m_impl->input,
                        m_impl->window,
                        m_impl->world.blockTarget
                            ? &*m_impl->world.blockTarget
                            : nullptr,
                        *m_impl->world.world,
                        *m_impl->world.worldView,
                        m_impl->world.placeBlock,
                        mutationMode);
                    if (worldEdited) {
                        refreshBlockTarget();
                    }
                    if (m_impl->world.galleryCatalog) {
                        m_impl->world.galleryTargetPresentation =
                            m_impl->world.blockTarget
                            ? Voxel::makeBlockGalleryTargetPresentation(
                                  *m_impl->world.galleryCatalog,
                                  m_impl->world.world->blockRegistry(),
                                  *m_impl->world.blockTarget)
                            : std::nullopt;
                    }
                    m_impl->world.world->tickEntities(deltaTime);
                }

                const auto [width, height] =
                    requireFramebufferSize(m_impl->runtime);

                {
                    PROFILE_SCOPE("Streaming");
                    {
                        PROFILE_SCOPE("Streaming/Update");
                        m_impl->world.worldView->updateStreaming(m_impl->camera.position);
                    }
                    {
                        PROFILE_SCOPE("Streaming/Apply");
                        m_impl->world.worldView->updateMeshes();
                    }
                    const auto& diagnostics =
                        m_impl->world.worldView->streamingDiagnostics();
                    if (!m_impl->world.streamingLifecycleLogged ||
                        diagnostics.state != m_impl->world.lastStreamingLifecycle ||
                        Voxel::streamingFailureSignatureChanged(
                            m_impl->world.lastStreamingDiagnostics,
                            diagnostics)) {
                        spdlog::info(
                            "streaming.lifecycle state={} "
                            "generation.pending={} generation.in_flight={} generation.started={} "
                            "generation.terminal_errors={} generation.last_error=\"{}\" "
                            "load.pending={} load.in_flight={} load.started={} "
                            "load.terminal_errors={} load.last_error=\"{}\" "
                            "mesh.pending={} mesh.in_flight={} mesh.workers={} "
                            "mesh.submission_limit={} mesh.started={} "
                            "mesh.terminal_errors={} mesh.last_error=\"{}\" "
                            "eviction.pending={} eviction.last_error=\"{}\" "
                            "stable_updates={}/{}",
                            Voxel::streamingLifecycleName(diagnostics.state),
                            diagnostics.generation.pending,
                            diagnostics.generation.inFlight,
                            diagnostics.generation.started,
                            diagnostics.generation.terminalErrors,
                            diagnostics.generation.lastError,
                            diagnostics.chunkLoad.pending,
                            diagnostics.chunkLoad.inFlight,
                            diagnostics.chunkLoad.started,
                            diagnostics.chunkLoad.terminalErrors,
                            diagnostics.chunkLoad.lastError,
                            diagnostics.mesh.pending,
                            diagnostics.mesh.inFlight,
                            diagnostics.meshWorkerCount,
                            diagnostics.meshSubmissionLimit,
                            diagnostics.mesh.started,
                            diagnostics.mesh.terminalErrors,
                            diagnostics.mesh.lastError,
                            diagnostics.eviction.pending,
                            diagnostics.eviction.lastError,
                            diagnostics.stableUpdates,
                            Voxel::StreamingDiagnosticSnapshot::QuiescenceUpdateWindow);
                        const auto& regions = diagnostics.regionScheduler;
                        spdlog::info(
                            "streaming.region_scheduler "
                            "direct.logical_admissions={} direct.retry_admissions={} "
                            "direct.pool_submissions={} direct.pool_resubmissions={} "
                            "direct.pool_starts={} direct.inline_executions={} "
                            "direct.pool_yields={} direct.terminal_pool_cancellations={} "
                            "direct.logical_pre_start_cancellations={} "
                            "direct.results_published={} direct.results_drained={} "
                            "direct.missing_probes={} "
                            "direct.admission_to_start_ns={} direct.max_admission_to_start_ns={} "
                            "direct.worker_execution_ns={} direct.max_worker_execution_ns={} "
                            "speculative.logical_admissions={} speculative.retry_admissions={} "
                            "speculative.pool_submissions={} speculative.pool_resubmissions={} "
                            "speculative.pool_starts={} speculative.inline_executions={} "
                            "speculative.pool_yields={} speculative.terminal_pool_cancellations={} "
                            "speculative.logical_pre_start_cancellations={} "
                            "speculative.results_published={} speculative.results_drained={} "
                            "speculative.missing_probes={} "
                            "speculative.admission_to_start_ns={} speculative.max_admission_to_start_ns={} "
                            "speculative.worker_execution_ns={} speculative.max_worker_execution_ns={} "
                            "demand_owned.queued={} demand_owned.dispatched_undrained={} "
                            "speculative_owned.queued={} speculative_owned.dispatched_undrained={} "
                            "speculative_pool.pending={} speculative_pool.max_pending={} "
                            "speculative_pool.yield_calls={} speculative_pool.yield_candidate_visits={} "
                            "speculative_pool.max_yield_candidate_visits={} "
                            "promotions={} useful_prefetch_hits={} evicted_before_demand={}",
                            regions.directOrigin.logicalAdmissions,
                            regions.directOrigin.retryAdmissions,
                            regions.directOrigin.poolSubmissions,
                            regions.directOrigin.poolResubmissions,
                            regions.directOrigin.poolWorkerStarts,
                            regions.directOrigin.inlineExecutions,
                            regions.directOrigin.successfulPoolYields,
                            regions.directOrigin.terminalPoolCancellations,
                            regions.directOrigin.logicalPreStartCancellations,
                            regions.directOrigin.resultsPublished,
                            regions.directOrigin.resultsDrained,
                            regions.directOrigin.missingProbes,
                            regions.directOrigin.admissionToWorkerStartNanoseconds,
                            regions.directOrigin.maxAdmissionToWorkerStartNanoseconds,
                            regions.directOrigin.workerExecutionNanoseconds,
                            regions.directOrigin.maxWorkerExecutionNanoseconds,
                            regions.speculativeOrigin.logicalAdmissions,
                            regions.speculativeOrigin.retryAdmissions,
                            regions.speculativeOrigin.poolSubmissions,
                            regions.speculativeOrigin.poolResubmissions,
                            regions.speculativeOrigin.poolWorkerStarts,
                            regions.speculativeOrigin.inlineExecutions,
                            regions.speculativeOrigin.successfulPoolYields,
                            regions.speculativeOrigin.terminalPoolCancellations,
                            regions.speculativeOrigin.logicalPreStartCancellations,
                            regions.speculativeOrigin.resultsPublished,
                            regions.speculativeOrigin.resultsDrained,
                            regions.speculativeOrigin.missingProbes,
                            regions.speculativeOrigin.admissionToWorkerStartNanoseconds,
                            regions.speculativeOrigin.maxAdmissionToWorkerStartNanoseconds,
                            regions.speculativeOrigin.workerExecutionNanoseconds,
                            regions.speculativeOrigin.maxWorkerExecutionNanoseconds,
                            regions.demandOwnedQueued,
                            regions.demandOwnedDispatchedUndrained,
                            regions.speculativeOwnedQueued,
                            regions.speculativeOwnedDispatchedUndrained,
                            regions.speculativePoolJobsPending,
                            regions.maxSpeculativePoolJobsPending,
                            regions.speculativePoolYieldCalls,
                            regions.speculativePoolYieldCandidateVisits,
                            regions.maxSpeculativePoolYieldCandidateVisits,
                            regions.demandPromotions,
                            regions.usefulPrefetchCacheHits,
                            regions.speculativeEvictionsBeforeDemand);
                        m_impl->world.lastStreamingLifecycle = diagnostics.state;
                        m_impl->world.lastStreamingDiagnostics = diagnostics;
                        m_impl->world.streamingLifecycleLogged = true;
                    }
                }

                {
                    PROFILE_SCOPE("Render");
                    m_impl->renderer.render(Render::FrameRenderContext{
                        *m_impl->world.world,
                        *m_impl->world.worldView,
                        m_impl->camera.position,
                        m_impl->camera.target,
                        m_impl->camera.forward,
                        width,
                        height,
                        deltaTime,
                        m_impl->world.blockTarget
                            ? &*m_impl->world.blockTarget
                            : nullptr});
#if defined(RIGEL_ENABLE_IMGUI)
                    UI::renderProfilerWindow(
                        m_impl->renderer.profilerWindowEnabled());
                    UI::renderChunkDebugLegend(
                        m_impl->renderer.debugOverlayEnabled(),
                        m_impl->renderer.chunkDebugDetail());
                    if (m_impl->worldMode == WorldMode::BlockGallery) {
                        UI::renderBlockGalleryTarget(
                            m_impl->world.galleryTargetPresentation
                                ? &*m_impl->world.galleryTargetPresentation
                                : nullptr);
                    }
#else
                    (void)width;
                    (void)height;
#endif
                }
            } else {
                const auto [width, height] =
                    requireFramebufferSize(m_impl->runtime);
                m_impl->renderer.clear(width, height);
            }
            ApplicationTestAccess::observeBlockGalleryLaunchFrame(*this);
        }
        Core::Profiler::endFrame();

        UI::endFrame();
        m_impl->runtime.swapBuffers(m_impl->window.window);
        m_impl->preferences->waitForNextFrame();

    }

    if (m_impl->timing.benchmarkEnabled) {
        double endTime = m_impl->runtime.time();
        double elapsed = endTime - m_impl->timing.benchmarkStartTime;
        const auto& stats = m_impl->timing.benchmark;
        double genRate = (elapsed > 0.0)
            ? static_cast<double>(stats.generatedChunks) / elapsed
            : 0.0;
        double processedRate = (elapsed > 0.0)
            ? static_cast<double>(stats.processedChunks()) / elapsed
            : 0.0;
        double meshedRate = (elapsed > 0.0)
            ? static_cast<double>(stats.meshedChunks) / elapsed
            : 0.0;
        spdlog::info(
            "Chunk benchmark (lifetime): generated {} ({:.1f}/s), processed {} ({:.1f}/s), "
            "meshed {} ({:.1f}/s), empty {}, wall {:.2f}s "
            "[gen {:.2f}s, mesh {:.2f}s, empty {:.2f}s]",
            stats.generatedChunks,
            genRate,
            stats.processedChunks(),
            processedRate,
            stats.meshedChunks,
            meshedRate,
            stats.emptyChunks,
            elapsed,
            stats.generationSeconds,
            stats.meshSeconds,
            stats.emptyMeshSeconds
        );
    }
}

PreferenceApplyResult Application::applyDisplayPreferences(
    const Preferences::DisplayPreferences& preferences,
    WindowedSizeIntent windowedSizeIntent) {
    return m_impl->preferences->applyDisplay(
        m_impl->runtime, preferences, windowedSizeIntent);
}

PreferenceApplyResult Application::applyVerticalFov(double verticalFovDegrees) {
    return m_impl->preferences->applyVerticalFov(
        m_impl->renderer, verticalFovDegrees);
}

PreferenceApplyResult Application::applyShadows(bool enabled) {
    if (!m_impl->preferences || !m_impl->world.ready ||
        !m_impl->world.worldView) {
        return {
            PreferenceApplyStatus::Rejected,
            "shadows require an active world session"};
    }
    return m_impl->preferences->applyShadows(
        *m_impl->world.worldView, enabled);
}

PreferenceApplyResult Application::applyViewDistance(int viewDistanceChunks) {
    if (!m_impl->preferences || !m_impl->world.ready ||
        !m_impl->world.worldView) {
        return {
            PreferenceApplyStatus::Rejected,
            "view distance requires an active world session"};
    }
    return m_impl->preferences->requestViewDistance(viewDistanceChunks);
}

PreferenceApplyResult Application::applyInputPreferences(
    const Preferences::InputPreferences& preferences) {
    return m_impl->preferences->applyInput(
        m_impl->input, *m_impl->playerDefaultBindings, preferences);
}

PreferenceApplyResult Application::resetControlBindings() {
    return m_impl->preferences->resetControlBindings(
        m_impl->input, *m_impl->playerDefaultBindings);
}

const Preferences::UserPreferences& Application::requestedPreferences() const {
    return m_impl->preferences->requested();
}

const Preferences::DisplayPreferences&
Application::effectiveDisplayPreferences() const {
    return m_impl->preferences->effectiveDisplay();
}

double Application::effectiveVerticalFovDegrees() const {
    return m_impl->preferences->effectiveVerticalFovDegrees();
}

bool Application::effectiveShadowsEnabled() const {
    return m_impl->preferences->effectiveShadowsEnabled();
}

int Application::effectiveViewDistanceChunks() const {
    return m_impl->preferences->effectiveViewDistanceChunks();
}

const Preferences::InputPreferences&
Application::effectiveInputPreferences() const {
    return m_impl->preferences->effectiveInput();
}

} // namespace Rigel
