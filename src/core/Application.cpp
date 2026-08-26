#include "Rigel/Application.h"
#include "ApplicationEntry.h"
#include "ApplicationTestAccess.h"
#include "GlfwRuntime.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/PersistenceConfigBootstrap.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Persistence/WorldSettings.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Render/OpenGLRuntime.h"
#include "Rigel/Render/RenderConfigBootstrap.h"
#include "Rigel/Voxel/ChunkBenchmark.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/GeneratorSnapshot.h"
#include "Rigel/Voxel/WorldSet.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Voxel/WorldConfigBootstrap.h"
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
        std::optional<Persistence::WorldSettings> settings;
        bool ready = false;
        bool streamingLifecycleLogged = false;
        Voxel::StreamingLifecycleState lastStreamingLifecycle =
            Voxel::StreamingLifecycleState::DiscoveringSpawn;
        Voxel::StreamingDiagnosticSnapshot lastStreamingDiagnostics;
        Voxel::BlockID placeBlock = Voxel::BlockRegistry::airId();
    };

    GlfwRuntime runtime;
    Asset::AssetManager assets;
    Input::WindowState window;
    Input::CameraState camera;
    Input::InputState input;
    Input::DebugOverlayListener debugOverlayListener;
    Input::ImGuiOverlayListener imguiOverlayListener;
    Render::FrameRenderer renderer;
    TimingState timing;
    WorldState world;
    Input::InputCallbackContext inputCallbacks;
    bool openGLInitialized = false;
    bool shutDown = false;
    void (*afterContextAcquired)() = nullptr;
    void (*shutdownStageCompleted)(ApplicationShutdownStage) noexcept = nullptr;

    Impl() = default;
    explicit Impl(ApplicationConstructionHooks hooks)
        : runtime(hooks.runtimeApi)
        , afterContextAcquired(hooks.afterContextAcquired)
        , shutdownStageCompleted(hooks.shutdownStageCompleted) {
    }
    ~Impl();
    void persistWorld();
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

    const char* benchEnv = std::getenv("RIGEL_CHUNK_BENCH");
    m_impl->timing.benchmarkEnabled =
        benchEnv && benchEnv[0] != '\0' && benchEnv[0] != '0';

    // Initialize GLFW
    if (!m_impl->runtime.initialize()) {
        spdlog::error("GLFW initialization failed");
        throw std::runtime_error("GLFW initialization failed");
    }
    spdlog::info("GLFW initialized successfully");

    // Create a simple GLFW window
    m_impl->runtime.windowHint(
        GLFW_CONTEXT_VERSION_MAJOR, Render::kOpenGLContextMajorVersion);
    m_impl->runtime.windowHint(
        GLFW_CONTEXT_VERSION_MINOR, Render::kOpenGLContextMinorVersion);
    m_impl->runtime.windowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    m_impl->runtime.windowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    m_impl->runtime.windowHint(GLFW_DEPTH_BITS, 24);

    m_impl->window.window = m_impl->runtime.createWindow(800, 600, "Rigel");
    if (!m_impl->window.window) {
        spdlog::error("Failed to create GLFW window");
        throw std::runtime_error("Failed to create GLFW window");
    }

    m_impl->runtime.makeContextCurrent();
    if (m_impl->afterContextAcquired) {
        m_impl->afterContextAcquired();
    }
    const int swapInterval = m_impl->timing.benchmarkEnabled ? 0 : 1;
    glfwSwapInterval(swapInterval);
    spdlog::info("Frame pacing swap interval: {}", swapInterval);

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        spdlog::error("GLEW initialization failed");
        throw std::runtime_error("GLEW initialization failed");
    }
    m_impl->openGLInitialized = true;
    spdlog::info("GLEW initialized successfully");

    // Print OpenGL version
    spdlog::info("OpenGL Version: {}", (char*)glGetString(GL_VERSION));

#if defined(RIGEL_ENABLE_IMGUI)
    initializeOptionalUserInterface(m_impl->window.window, &UI::init);
#endif

    // Set initial viewport
    glViewport(0, 0, 800, 600);

    // Set Callbacks
    glfwSetFramebufferSizeCallback(m_impl->window.window, [](GLFWwindow* window, int width, int height)-> void {
        glViewport(0, 0, width, height);
    });
    m_impl->inputCallbacks.input = &m_impl->input;
    m_impl->inputCallbacks.window = &m_impl->window;
    m_impl->inputCallbacks.camera = &m_impl->camera;
    Input::registerWindowCallbacks(m_impl->window.window, m_impl->inputCallbacks);
    Input::setCursorCaptured(m_impl->window, true);
    if (m_impl->timing.benchmarkEnabled) {
        spdlog::info("Chunk benchmark enabled");
    }

    try {
        m_impl->assets.loadManifest("manifest.yaml");
        m_impl->assets.registerLoader("input", std::make_unique<Input::InputBindingsLoader>());
        m_impl->assets.registerLoader("entity_models", std::make_unique<Entity::EntityModelLoader>());
        m_impl->assets.registerLoader("entity_anims", std::make_unique<Entity::EntityAnimationSetLoader>());
        m_impl->world.worldSet.persistenceFormats().registerFormat(
            Persistence::Backends::CR::descriptor(),
            Persistence::Backends::CR::factory(),
            Persistence::Backends::CR::probe());
        m_impl->world.worldSet.persistenceFormats().registerFormat(
            Persistence::Backends::Memory::descriptor(),
            Persistence::Backends::Memory::factory(),
            Persistence::Backends::Memory::probe());
        m_impl->world.worldSet.setPersistenceStorage(std::make_shared<Persistence::FilesystemBackend>());
        m_impl->world.worldSet.setPersistenceRoot(
            Persistence::mainWorldRootPath(m_impl->world.activeWorldId));
        Persistence::PersistenceConfigProvider persistenceConfigProvider =
            Persistence::makePersistenceConfigProvider(
                m_impl->assets, m_impl->world.activeWorldId);
        Persistence::PersistenceConfig persistenceConfig =
            persistenceConfigProvider.load();
        if (!persistenceConfig.format.empty()) {
            m_impl->world.worldSet.setPersistencePreferredFormat(
                persistenceConfig.format);
        }
        Voxel::WorldConfigProvider configProvider =
            Voxel::makeWorldConfigProvider(m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldConfiguration config;
        Render::RenderConfigProvider renderConfigProvider =
            Render::makeRenderConfigProvider(
                m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldRenderConfig renderConfig = renderConfigProvider.load();

        m_impl->world.worldSet.initializeResources(m_impl->assets);

        Input::loadInputBindings(m_impl->assets, m_impl->input);
        m_impl->debugOverlayListener.enabled = &m_impl->renderer.debugOverlayEnabled();
        m_impl->input.addListener(&m_impl->debugOverlayListener);
        m_impl->imguiOverlayListener.enabled = &m_impl->renderer.profilerWindowEnabled();
        m_impl->input.addListener(&m_impl->imguiOverlayListener);

        m_impl->world.world = &m_impl->world.worldSet.createWorld(m_impl->world.activeWorldId);
        m_impl->world.worldView = &m_impl->world.worldSet.createView(m_impl->world.activeWorldId, m_impl->assets);

        auto crSettings = std::make_shared<
            Persistence::Backends::CR::CRPersistenceSettings>();
        crSettings->enableLz4 = persistenceConfig.crLz4Enabled;
        m_impl->world.world->persistenceProviders().add(
            Persistence::Backends::CR::kCRSettingsProviderId,
            crSettings);

        Persistence::PersistenceContext persistenceContext =
            m_impl->world.worldSet.persistenceContext(m_impl->world.activeWorldId);
        std::shared_ptr<const Voxel::WorldGenerator> generator;
        Persistence::recoverAbandonedWorldGenerationStaging(
            persistenceContext);
        const auto savedPresence =
            Persistence::inspectSavedWorldGeneration(persistenceContext);
        persistenceContext.discoverExistingFormat =
            savedPresence ==
            Persistence::SavedWorldGenerationPresence::Published;
        if (savedPresence ==
            Persistence::SavedWorldGenerationPresence::Missing) {
            config = configProvider.loadConfig();
            Voxel::validateGeneratorSnapshotContent(
                config.generation,
                m_impl->world.worldSet.resources().registry());
            config.generation.world.version =
                Voxel::kGeneratorSemanticsVersion;
            generator = std::make_shared<const Voxel::WorldGenerator>(
                m_impl->world.worldSet.resources().registry(),
                config.generation);
            Persistence::WorldSettings settings;
            settings.displayName =
                "world_" + std::to_string(m_impl->world.activeWorldId);
            settings.seed = generator->config().seed;
            settings.generator.sourceId = config.generatorSource.id;
            settings.generator.sourceRevision =
                config.generatorSource.revision;
            settings.generator.definitionSchemaVersion =
                Voxel::kGeneratorDefinitionSchemaVersion;
            settings.generator.semanticsVersion =
                Voxel::kGeneratorSemanticsVersion;
            Persistence::publishNewWorldGeneration(
                settings, generator->config(), persistenceContext);
            m_impl->world.settings = std::move(settings);
        } else if (savedPresence ==
                   Persistence::SavedWorldGenerationPresence::Published) {
            config.streaming = configProvider.loadStreamingConfig();
            Persistence::SavedWorldGeneration saved =
                Persistence::loadSavedWorldGeneration(persistenceContext);
            config.generation = std::move(saved.definition);
            Voxel::validateGeneratorSnapshotContent(
                config.generation,
                m_impl->world.worldSet.resources().registry());
            generator = std::make_shared<const Voxel::WorldGenerator>(
                m_impl->world.worldSet.resources().registry(),
                config.generation);
            m_impl->world.settings = std::move(saved.settings);
        } else {
            throw std::runtime_error(
                "Existing world is legacy, unknown, or incompletely published; "
                "the save was left unchanged");
        }

        auto& persistenceService =
            m_impl->world.worldSet.persistenceService();
        auto persistenceFormat = persistenceService.openFormat(
            persistenceContext);
        persistenceContext.preferredFormat =
            persistenceFormat->descriptor().id;
        persistenceContext.discoverExistingFormat = false;
        m_impl->world.worldSet.setPersistenceActiveFormat(
            m_impl->world.activeWorldId,
            persistenceContext.preferredFormat);
        const std::string backendMetadataPath =
            persistenceFormat->worldMetadataCodec().metadataPath(
                persistenceContext);
        if (!persistenceContext.storage->exists(backendMetadataPath)) {
            Persistence::WorldMetadata backendMetadata;
            backendMetadata.worldId =
                "world_" + std::to_string(m_impl->world.activeWorldId);
            backendMetadata.displayName = m_impl->world.settings->displayName;
            persistenceService.saveWorldMetadata(
                backendMetadata, persistenceContext);
        }

        m_impl->world.world->setGenerator(generator);
        m_impl->world.worldView->setGenerator(generator);

        Persistence::loadBootstrapEntities(
            *m_impl->world.world,
            m_impl->assets,
            m_impl->world.worldSet.persistenceService(),
            persistenceContext);

        uint32_t worldGenVersion = generator->config().world.version;
        size_t ioThreads = static_cast<size_t>(
            std::max(0, config.streaming.ioThreads));
        size_t loadWorkerThreads = static_cast<size_t>(
            std::max(0, config.streaming.loadWorkerThreads));
        m_impl->world.chunkLoader = std::make_shared<Persistence::AsyncChunkLoader>(
            m_impl->world.worldSet.persistenceService(),
            std::move(persistenceContext),
            *m_impl->world.world,
            worldGenVersion,
            ioThreads,
            loadWorkerThreads,
            config.streaming.viewDistanceChunks,
            generator);
        if (config.streaming.loadQueueLimit >= 0) {
            m_impl->world.chunkLoader->setLoadQueueLimit(
                static_cast<size_t>(config.streaming.loadQueueLimit));
        }
        m_impl->world.chunkLoader->setRegionDrainBudget(
            static_cast<size_t>(
                std::max(0, config.streaming.loadRegionDrainBudget)));
        m_impl->world.chunkLoader->setMaxCachedRegions(
            static_cast<size_t>(
                std::max(0, config.streaming.loadMaxCachedRegions)));
        m_impl->world.chunkLoader->setMaxInFlightRegions(
            static_cast<size_t>(
                std::max(0, config.streaming.loadMaxInFlightRegions)));
        m_impl->world.chunkLoader->setPrefetchRadius(
            std::max(0, config.streaming.loadPrefetchRadius));
        m_impl->world.chunkLoader->setPrefetchPerRequest(
            static_cast<size_t>(
                std::max(0, config.streaming.loadPrefetchPerRequest)));
        m_impl->world.worldView->setChunkLoader(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkLoadRequest request) {
                return loader
                    ? loader->request(request)
                    : Voxel::ChunkLoadRequestResult::Missing;
            });
        m_impl->world.worldView->setChunkPendingCallback(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader ? loader->isPending(coord) : false;
            });
        m_impl->world.worldView->setChunkLoadDrain(
            [loader = m_impl->world.chunkLoader](size_t budget) {
                if (loader) {
                    return loader->drainCompletions(budget);
                }
                return std::vector<Voxel::ChunkLoadCompletion>{};
            });
        m_impl->world.worldView->setChunkLoadCancel(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                if (loader) {
                    loader->cancel(coord);
                }
            });
        m_impl->world.worldView->setChunkLoadDiagnosticsCallback(
            [loader = m_impl->world.chunkLoader]() {
                return loader
                    ? loader->diagnostics()
                    : Voxel::ChunkLoadDiagnosticSnapshot{};
            });
        m_impl->world.worldView->setChunkLoadExecutionStateCallback(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader
                    ? loader->executionState(coord)
                    : std::optional<Voxel::ChunkLoadExecutionState>{};
            });
        m_impl->world.worldView->setChunkEvictionCallback(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader ? loader->persistChunk(coord) : false;
            });

        const char* profileEnv = std::getenv("RIGEL_PROFILE");
        if (profileEnv && profileEnv[0] != '\0') {
            renderConfig.profilingEnabled = (profileEnv[0] != '0');
        }
        m_impl->world.worldView->renderConfig() = renderConfig;
        Core::Profiler::setEnabled(renderConfig.profilingEnabled);
        m_impl->world.worldView->setStreamConfig(config.streaming);
        if (m_impl->timing.benchmarkEnabled) {
            m_impl->world.worldView->setBenchmark(&m_impl->timing.benchmark);
        }

        auto placeId = m_impl->world.world->blockRegistry().findByIdentifier(
            config.generation.solidBlock);
        if (!placeId) {
            placeId = m_impl->world.world->blockRegistry().findByIdentifier("base:stone_shale");
        }
        if (placeId) {
            m_impl->world.placeBlock = *placeId;
        } else if (m_impl->world.world->blockRegistry().size() > 1) {
            m_impl->world.placeBlock = Voxel::BlockID{1};
        }

        int spawnX = static_cast<int>(std::floor(m_impl->camera.position.x));
        int spawnZ = static_cast<int>(std::floor(m_impl->camera.position.z));
        int spawnY = Voxel::findFirstAirY(
            *generator, config.generation, spawnX, spawnZ);
        m_impl->camera.position.y = static_cast<float>(spawnY) + 0.5f;
        m_impl->world.worldView->markSpawnDiscoveryComplete();

        m_impl->renderer.initialize(m_impl->assets);
        m_impl->world.ready = true;
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

void Application::Impl::close() {
    if (shutDown) {
        return;
    }

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
        activeView->setChunkLoader({});
        activeView->setChunkPendingCallback({});
        activeView->setChunkLoadDrain({});
        activeView->setChunkLoadCancel({});
        activeView->setChunkLoadDiagnosticsCallback({});
        activeView->setChunkLoadExecutionStateCallback({});
        activeView->setChunkEvictionCallback({});
    }
    world.chunkLoader.reset();
    completeShutdownStage(ApplicationShutdownStage::AsyncLoadingStopped);

    if (activeView) {
        activeView->clear();
        if (openGLInitialized && hasContext) {
            activeView->releaseRenderResources();
        }
    }
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
    Application application(std::make_unique<Application::Impl>(hooks));
}

void ApplicationTestAccess::constructAndRun(
    ApplicationConstructionHooks hooks,
    void (*runLoop)(Application&)
) {
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
    Persistence::WorldSettings settings;
    settings.displayName = "Application Close Test World";
    impl->world.settings = std::move(settings);
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

int runApplication() noexcept {
    return runApplication([] {
        Application application;
        application.run();
        application.close();
    });
}

void Application::run() {
    m_impl->timing.lastTime = glfwGetTime();
    if (m_impl->timing.benchmarkEnabled) {
        m_impl->timing.benchmarkStartTime = m_impl->timing.lastTime;
    }

    // Render loop
    while (!glfwWindowShouldClose(m_impl->window.window)) {
        double now = glfwGetTime();
        float deltaTime = static_cast<float>(now - m_impl->timing.lastTime);
        m_impl->timing.lastTime = now;

        // Flush event queue
        glfwPollEvents();
        if (m_impl->window.pendingTimeReset) {
            m_impl->timing.lastTime = glfwGetTime();
            deltaTime = 0.0f;
            m_impl->window.pendingTimeReset = false;
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
                if (m_impl->window.cursorCaptured &&
                    glfwGetInputMode(m_impl->window.window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED) {
                    Input::setCursorCaptured(m_impl->window, true);
                }

                {
                    PROFILE_SCOPE("Simulation");
                    Input::updateCamera(m_impl->input, m_impl->camera, deltaTime);
                    Input::handleDemoSpawn(m_impl->input, m_impl->assets, *m_impl->world.world, m_impl->camera);
                    Input::handleBlockEdits(m_impl->input,
                                            m_impl->window,
                                            m_impl->camera,
                                            *m_impl->world.world,
                                            *m_impl->world.worldView,
                                            m_impl->world.placeBlock);
                    m_impl->world.world->tickEntities(deltaTime);
                }

                int width = 0;
                int height = 0;
                glfwGetFramebufferSize(m_impl->window.window, &width, &height);

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
                        deltaTime});
#if defined(RIGEL_ENABLE_IMGUI)
                    UI::renderProfilerWindow(
                        m_impl->renderer.profilerWindowEnabled());
                    UI::renderChunkDebugLegend(
                        m_impl->renderer.debugOverlayEnabled(),
                        m_impl->renderer.chunkDebugDetail());
#else
                    (void)width;
                    (void)height;
#endif
                }
            } else {
                int width = 0;
                int height = 0;
                glfwGetFramebufferSize(m_impl->window.window, &width, &height);
                m_impl->renderer.clear(width, height);
            }
        }
        Core::Profiler::endFrame();

        UI::endFrame();
        glfwSwapBuffers(m_impl->window.window);

    }

    if (m_impl->timing.benchmarkEnabled) {
        double endTime = glfwGetTime();
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

} // namespace Rigel
