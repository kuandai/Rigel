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
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Render/OpenGLRuntime.h"
#include "Rigel/Render/RenderConfigBootstrap.h"
#include "Rigel/Voxel/ChunkBenchmark.h"
#include "Rigel/Voxel/ChunkTasks.h"
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
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rigel {

namespace {

constexpr float kMaxFrameTime = 0.05f;

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
        bool ready = false;
        bool streamingLifecycleLogged = false;
        Voxel::StreamingLifecycleState lastStreamingLifecycle =
            Voxel::StreamingLifecycleState::DiscoveringSpawn;
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
    void shutdown(bool saveWorld) noexcept;
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
    : m_impl(std::move(impl)) {
    try {
        initialize();
    } catch (...) {
        m_impl->shutdown(false);
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
    if (!UI::init(m_impl->window.window)) {
        spdlog::warn("ImGui initialization failed");
    }
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
            m_impl->world.worldSet.setPersistencePreferredFormat(persistenceConfig.format);
        }
        m_impl->world.worldSet.initializeResources(m_impl->assets);

        Input::loadInputBindings(m_impl->assets, m_impl->input);
        m_impl->debugOverlayListener.enabled = &m_impl->renderer.debugOverlayEnabled();
        m_impl->input.addListener(&m_impl->debugOverlayListener);
        m_impl->imguiOverlayListener.enabled = &m_impl->renderer.profilerWindowEnabled();
        m_impl->input.addListener(&m_impl->imguiOverlayListener);

        Voxel::WorldConfigProvider configProvider =
            Voxel::makeWorldConfigProvider(m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldConfiguration config = configProvider.loadConfig();
        if (config.generation.solidBlock.empty()) {
            config.generation.solidBlock = "base:stone_shale";
        }
        if (config.generation.surfaceBlock.empty()) {
            config.generation.surfaceBlock = "base:grass";
        }

        m_impl->world.world = &m_impl->world.worldSet.createWorld(m_impl->world.activeWorldId);
        m_impl->world.worldView = &m_impl->world.worldSet.createView(m_impl->world.activeWorldId, m_impl->assets);

        if (const auto* provider = persistenceConfig.findProvider(Persistence::Backends::CR::kCRSettingsProviderId)) {
            auto crSettings = std::make_shared<Persistence::Backends::CR::CRPersistenceSettings>();
            crSettings->enableLz4 = provider->getBool("lz4", crSettings->enableLz4);
            m_impl->world.world->persistenceProviders().add(
                Persistence::Backends::CR::kCRSettingsProviderId,
                crSettings);
        }

        auto generator =
            std::make_shared<Voxel::WorldGenerator>(m_impl->world.worldSet.resources().registry());
        generator->setConfig(config.generation);
        m_impl->world.world->setGenerator(generator);
        m_impl->world.worldView->setGenerator(generator);

        Persistence::PersistenceContext persistenceContext =
            m_impl->world.worldSet.persistenceContext(m_impl->world.activeWorldId);
        Persistence::loadWorldFromDisk(
            *m_impl->world.world,
            m_impl->assets,
            m_impl->world.worldSet.persistenceService(),
            persistenceContext,
            generator->config().world.version,
            Persistence::LoadScope::EntitiesOnly);

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
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader
                    ? loader->request(coord)
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
        m_impl->world.worldView->setChunkLoadWorkCallback(
            [loader = m_impl->world.chunkLoader]() {
                return loader
                    ? loader->workCount()
                    : Voxel::StreamingWorkCount{};
            });
        m_impl->world.worldView->setChunkEvictionCallback(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader ? loader->persistChunk(coord) : false;
            });

        Render::RenderConfigProvider renderConfigProvider =
            Render::makeRenderConfigProvider(
                m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldRenderConfig renderConfig = renderConfigProvider.load();
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
    shutdown(false);
}

void Application::Impl::shutdown(bool saveWorld) noexcept {
    if (std::exchange(shutDown, true)) {
        return;
    }

    if (saveWorld && world.ready && world.world) {
        try {
            Persistence::saveWorldToDisk(
                *world.world,
                world.worldSet.persistenceService(),
                world.worldSet.persistenceContext(world.activeWorldId));
        } catch (const std::exception& e) {
            spdlog::error("World save failed: {}", e.what());
        }
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
        activeView->setChunkLoadWorkCallback({});
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
    spdlog::info("Application terminated successfully");
}

Application::~Application() {
    if (m_impl) {
        m_impl->shutdown(true);
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

int runApplication(ApplicationMain applicationMain) noexcept {
    try {
        applicationMain();
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
                        diagnostics.state != m_impl->world.lastStreamingLifecycle) {
                        spdlog::info(
                            "streaming.lifecycle state={} "
                            "generation.pending={} generation.in_flight={} generation.started={} "
                            "load.pending={} load.in_flight={} load.started={} "
                            "mesh.pending={} mesh.in_flight={} mesh.started={} "
                            "stable_updates={}/{}",
                            Voxel::streamingLifecycleName(diagnostics.state),
                            diagnostics.generation.pending,
                            diagnostics.generation.inFlight,
                            diagnostics.generation.started,
                            diagnostics.chunkLoad.pending,
                            diagnostics.chunkLoad.inFlight,
                            diagnostics.chunkLoad.started,
                            diagnostics.mesh.pending,
                            diagnostics.mesh.inFlight,
                            diagnostics.mesh.started,
                            diagnostics.stableUpdates,
                            Voxel::StreamingDiagnosticSnapshot::QuiescenceUpdateWindow);
                        m_impl->world.lastStreamingLifecycle = diagnostics.state;
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
