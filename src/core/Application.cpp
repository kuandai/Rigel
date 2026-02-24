#include "Rigel/Application.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Persistence/AsyncChunkLoader.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/ChunkBenchmark.h"
#include "Rigel/Voxel/ChunkTasks.h"
#include "Rigel/Voxel/VoxelLod/PersistenceSource.h"
#include "Rigel/Voxel/WorldSet.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Persistence/WorldPersistence.h"
#include "Rigel/Render/DebugOverlay.h"
#include "Rigel/Voxel/WorldConfigBootstrap.h"
#include "Rigel/Voxel/WorldSpawn.h"
#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/input/GameplayInput.h"
#include <spdlog/spdlog.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/input/InputDispatcher.h"
#include "Rigel/input/keypress.h"
#include "Rigel/version.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
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

float halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

} // namespace

struct Application::Impl {
    struct TemporalAA {
        GLuint sceneFbo = 0;
        GLuint sceneColor = 0;
        GLuint sceneDepth = 0;
        GLuint resolveFbo = 0;
        std::array<GLuint, 2> history{};
        std::array<GLuint, 2> historyDepth{};
        GLuint quadVao = 0;
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locCurrentColor = -1;
        GLint locCurrentDepth = -1;
        GLint locHistory = -1;
        GLint locHistoryDepth = -1;
        GLint locCurrentJitter = -1;
        GLint locInvViewProjection = -1;
        GLint locPrevViewProjection = -1;
        GLint locHistoryBlend = -1;
        GLint locHistoryValid = -1;
        GLint locTexelSize = -1;
        int width = 0;
        int height = 0;
        int historyIndex = 0;
        bool initialized = false;
        bool historyValid = false;
        glm::mat4 prevViewProjection{1.0f};
        uint64_t frameIndex = 0;
    };

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
        Voxel::BlockID placeBlock = Voxel::BlockRegistry::airId();
    };

    struct RenderState {
        TemporalAA taa;
    };

    Asset::AssetManager assets;
    Input::WindowState window;
    Input::CameraState camera;
    Input::InputState input;
    Render::DebugState debug;
    TimingState timing;
    WorldState world;
    RenderState render;
    Input::InputCallbackContext inputCallbacks;

    void releaseTaaTargets() {
        auto& taa = render.taa;
        if (taa.sceneFbo != 0) {
            glDeleteFramebuffers(1, &taa.sceneFbo);
            taa.sceneFbo = 0;
        }
        if (taa.resolveFbo != 0) {
            glDeleteFramebuffers(1, &taa.resolveFbo);
            taa.resolveFbo = 0;
        }
        if (taa.sceneColor != 0) {
            glDeleteTextures(1, &taa.sceneColor);
            taa.sceneColor = 0;
        }
        if (taa.sceneDepth != 0) {
            glDeleteTextures(1, &taa.sceneDepth);
            taa.sceneDepth = 0;
        }
        for (GLuint& tex : taa.history) {
            if (tex != 0) {
                glDeleteTextures(1, &tex);
                tex = 0;
            }
        }
        for (GLuint& tex : taa.historyDepth) {
            if (tex != 0) {
                glDeleteTextures(1, &tex);
                tex = 0;
            }
        }
        taa.width = 0;
        taa.height = 0;
        taa.historyValid = false;
    }

    void initTaa() {
        auto& taa = render.taa;
        if (taa.initialized) {
            return;
        }
        try {
            taa.shader = assets.get<Asset::ShaderAsset>("shaders/taa_resolve");
        } catch (const std::exception& e) {
            spdlog::warn("TAA shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &taa.quadVao);
        glBindVertexArray(taa.quadVao);
        glBindVertexArray(0);

        taa.locCurrentColor = taa.shader->uniform("u_currentColor");
        taa.locCurrentDepth = taa.shader->uniform("u_currentDepth");
        taa.locHistory = taa.shader->uniform("u_historyColor");
        taa.locHistoryDepth = taa.shader->uniform("u_historyDepth");
        taa.locCurrentJitter = taa.shader->uniform("u_currentJitter");
        taa.locInvViewProjection = taa.shader->uniform("u_invViewProjection");
        taa.locPrevViewProjection = taa.shader->uniform("u_prevViewProjection");
        taa.locHistoryBlend = taa.shader->uniform("u_historyBlend");
        taa.locHistoryValid = taa.shader->uniform("u_historyValid");
        taa.locTexelSize = taa.shader->uniform("u_texelSize");

        taa.initialized = true;
    }

    void ensureTaaTargets(int width, int height) {
        auto& taa = render.taa;
        initTaa();
        if (!taa.initialized || width <= 0 || height <= 0) {
            return;
        }
        if (taa.width == width && taa.height == height && taa.sceneFbo != 0) {
            return;
        }

        releaseTaaTargets();

        taa.width = width;
        taa.height = height;
        taa.historyValid = false;

        glGenTextures(1, &taa.sceneColor);
        glBindTexture(GL_TEXTURE_2D, taa.sceneColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &taa.sceneDepth);
        glBindTexture(GL_TEXTURE_2D, taa.sceneDepth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        glGenTextures(2, taa.history.data());
        for (GLuint& tex : taa.history) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glGenTextures(2, taa.historyDepth.data());
        for (GLuint& tex : taa.historyDepth) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                         GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }

        glGenFramebuffers(1, &taa.sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, taa.sceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, taa.sceneColor, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, taa.sceneDepth, 0);
        GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::warn("TAA scene FBO incomplete: status=0x{:X}", status);
        }

        glGenFramebuffers(1, &taa.resolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glm::vec2 nextJitter(int width, int height, float scale) {
        auto& taa = render.taa;
        if (width <= 0 || height <= 0) {
            return glm::vec2(0.0f);
        }
        ++taa.frameIndex;
        float jx = halton(static_cast<uint32_t>(taa.frameIndex), 2) - 0.5f;
        float jy = halton(static_cast<uint32_t>(taa.frameIndex), 3) - 0.5f;
        float offsetX = jx * scale * 2.0f / static_cast<float>(width);
        float offsetY = jy * scale * 2.0f / static_cast<float>(height);
        return glm::vec2(offsetX, offsetY);
    }

    bool resolveTaa(const glm::mat4& invViewProjection,
                    const glm::mat4& viewProjection,
                    const glm::vec2& jitterUv,
                    float blend) {
        auto& taa = render.taa;
        if (!taa.initialized || taa.resolveFbo == 0 || taa.sceneColor == 0) {
            return false;
        }

        int readIndex = taa.historyIndex;
        int writeIndex = (taa.historyIndex + 1) % 2;

        glBindFramebuffer(GL_FRAMEBUFFER, taa.resolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, taa.history[writeIndex], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, taa.historyDepth[writeIndex], 0);
        GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);

        taa.shader->bind();
        if (taa.locCurrentColor >= 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, taa.sceneColor);
            glUniform1i(taa.locCurrentColor, 0);
        }
        if (taa.locCurrentDepth >= 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, taa.sceneDepth);
            glUniform1i(taa.locCurrentDepth, 1);
        }
        if (taa.locHistory >= 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, taa.history[readIndex]);
            glUniform1i(taa.locHistory, 2);
        }
        if (taa.locHistoryDepth >= 0) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, taa.historyDepth[readIndex]);
            glUniform1i(taa.locHistoryDepth, 3);
        }
        if (taa.locCurrentJitter >= 0) {
            glUniform2fv(taa.locCurrentJitter, 1, glm::value_ptr(jitterUv));
        }
        if (taa.locInvViewProjection >= 0) {
            glUniformMatrix4fv(taa.locInvViewProjection, 1, GL_FALSE,
                               glm::value_ptr(invViewProjection));
        }
        if (taa.locPrevViewProjection >= 0) {
            glUniformMatrix4fv(taa.locPrevViewProjection, 1, GL_FALSE,
                               glm::value_ptr(taa.prevViewProjection));
        }
        if (taa.locHistoryBlend >= 0) {
            glUniform1f(taa.locHistoryBlend, blend);
        }
        if (taa.locHistoryValid >= 0) {
            glUniform1i(taa.locHistoryValid, taa.historyValid ? 1 : 0);
        }
        if (taa.locTexelSize >= 0) {
            glm::vec2 texelSize(1.0f / static_cast<float>(taa.width),
                                1.0f / static_cast<float>(taa.height));
            glUniform2fv(taa.locTexelSize, 1, glm::value_ptr(texelSize));
        }

        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);

        glBindVertexArray(taa.quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.resolveFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, taa.resolveFbo);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        taa.historyValid = true;
        taa.historyIndex = writeIndex;
        taa.prevViewProjection = viewProjection;

        return true;
    }

};

Application::Application() : m_impl(std::make_unique<Impl>()) {
    #ifdef DEBUG
    if (!std::string_view(RIGEL_GIT_HASH).empty()) {
        spdlog::info("Rigel v{} Developer Preview (git {})", RIGEL_VERSION, RIGEL_GIT_HASH);
    } else {
        spdlog::info("Rigel v{} Developer Preview", RIGEL_VERSION);
    }
    #else
    spdlog::info("Rigel v{}", RIGEL_VERSION);
    #endif
    spdlog::info("Optional components: {}", RIGEL_OPTIONAL_COMPONENTS);

    // Initialize GLFW
    if (!glfwInit()) {
        spdlog::error("GLFW initialization failed");
        throw std::runtime_error("GLFW initialization failed");
    }
    spdlog::info("GLFW initialized successfully");

    // Create a simple GLFW window
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);

    m_impl->window.window = glfwCreateWindow(800, 600, "Rigel", nullptr, nullptr);
    if (!m_impl->window.window) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_impl->window.window);

    // Initialize GLEW
    if (glewInit() != GLEW_OK) {
        spdlog::error("GLEW initialization failed");
        glfwDestroyWindow(m_impl->window.window);
        glfwTerminate();
        throw std::runtime_error("GLEW initialization failed");
    }
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
    m_impl->inputCallbacks.window = &m_impl->window;
    m_impl->inputCallbacks.camera = &m_impl->camera;
    Input::registerWindowCallbacks(m_impl->window.window, m_impl->inputCallbacks);
    Input::setCursorCaptured(m_impl->window, true);
    const char* benchEnv = std::getenv("RIGEL_CHUNK_BENCH");
    if (benchEnv && benchEnv[0] != '\0' && benchEnv[0] != '0') {
        m_impl->timing.benchmarkEnabled = true;
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
        Voxel::ConfigProvider persistenceConfigProvider =
            Voxel::makePersistenceConfigProvider(m_impl->assets, m_impl->world.activeWorldId);
        Persistence::PersistenceConfig persistenceConfig = persistenceConfigProvider.loadPersistenceConfig();
        if (!persistenceConfig.format.empty()) {
            m_impl->world.worldSet.setPersistencePreferredFormat(persistenceConfig.format);
        }
        m_impl->world.worldSet.initializeResources(m_impl->assets);

        Input::loadInputBindings(m_impl->assets, m_impl->input);
        Input::attachDebugOverlayListener(m_impl->input, &m_impl->debug.overlayEnabled);
        Input::attachImGuiOverlayListener(m_impl->input, &m_impl->debug.imguiEnabled);

        Voxel::ConfigProvider configProvider =
            Voxel::makeWorldConfigProvider(m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldGenConfig config = configProvider.loadConfig();
        if (config.solidBlock.empty()) {
            config.solidBlock = "base:stone_shale";
        }
        if (config.surfaceBlock.empty()) {
            config.surfaceBlock = "base:grass";
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
        generator->setConfig(config);
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
            Persistence::SaveScope::EntitiesOnly);

        uint32_t worldGenVersion = generator->config().world.version;
        size_t ioThreads = static_cast<size_t>(std::max(0, config.stream.ioThreads));
        size_t loadWorkerThreads = static_cast<size_t>(std::max(0, config.stream.loadWorkerThreads));
        m_impl->world.chunkLoader = std::make_shared<Persistence::AsyncChunkLoader>(
            m_impl->world.worldSet.persistenceService(),
            std::move(persistenceContext),
            *m_impl->world.world,
            worldGenVersion,
            ioThreads,
            loadWorkerThreads,
            config.stream.viewDistanceChunks,
            generator);
        if (config.stream.loadQueueLimit >= 0) {
            m_impl->world.chunkLoader->setLoadQueueLimit(
                static_cast<size_t>(config.stream.loadQueueLimit));
        }
        m_impl->world.chunkLoader->setRegionDrainBudget(
            static_cast<size_t>(std::max(0, config.stream.loadRegionDrainBudget)));
        m_impl->world.chunkLoader->setMaxCachedRegions(
            static_cast<size_t>(std::max(0, config.stream.loadMaxCachedRegions)));
        m_impl->world.chunkLoader->setMaxInFlightRegions(
            static_cast<size_t>(std::max(0, config.stream.loadMaxInFlightRegions)));
        m_impl->world.chunkLoader->setPrefetchRadius(
            std::max(0, config.stream.loadPrefetchRadius));
        m_impl->world.chunkLoader->setPrefetchPerRequest(
            static_cast<size_t>(std::max(0, config.stream.loadPrefetchPerRequest)));
        m_impl->world.worldView->setChunkLoader(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader ? loader->request(coord) : false;
            });
        m_impl->world.worldView->setChunkPendingCallback(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                return loader ? loader->isPending(coord) : false;
            });
        m_impl->world.worldView->setChunkLoadDrain(
            [loader = m_impl->world.chunkLoader](size_t budget) {
                if (loader) {
                    loader->drainCompletions(budget);
                }
            });
        m_impl->world.worldView->setChunkLoadCancel(
            [loader = m_impl->world.chunkLoader](Voxel::ChunkCoord coord) {
                if (loader) {
                    loader->cancel(coord);
                }
            });
        // Do not invalidate voxel-SVO pages for ordinary chunk streaming applies.
        // Stream-populated chunks already come from the same persistence/generator sources
        // the voxel-SVO sampler reads, and invalidating here causes continuous churn while
        // the stream is filling. Runtime voxel edits still invalidate explicitly.
        m_impl->world.chunkLoader->setChunkAppliedCallback({});
        auto persistenceSource = std::make_shared<Voxel::PersistenceSource>(
            &m_impl->world.worldSet.persistenceService(),
            m_impl->world.worldSet.persistenceContext(m_impl->world.activeWorldId));
        const size_t cachedRegions =
            static_cast<size_t>(std::max(1, config.stream.loadMaxCachedRegions));
        const size_t cachedChunksPerRegion =
            static_cast<size_t>(Voxel::Chunk::SIZE);
        persistenceSource->setCacheLimits(
            cachedRegions,
            cachedRegions * cachedChunksPerRegion);
        m_impl->world.worldView->setVoxelPersistenceSource(std::move(persistenceSource));

        Voxel::ConfigProvider renderConfigProvider =
            Voxel::makeRenderConfigProvider(m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldRenderConfig renderConfig = renderConfigProvider.loadRenderConfig();
        const char* profileEnv = std::getenv("RIGEL_PROFILE");
        if (profileEnv && profileEnv[0] != '\0') {
            renderConfig.profilingEnabled = (profileEnv[0] != '0');
        }
        m_impl->world.worldView->setRenderConfig(renderConfig);
        Core::Profiler::setEnabled(renderConfig.profilingEnabled);
        m_impl->world.worldView->setStreamConfig(config.stream);
        if (m_impl->timing.benchmarkEnabled) {
            m_impl->world.worldView->setBenchmark(&m_impl->timing.benchmark);
        }

        auto placeId = m_impl->world.world->blockRegistry().findByIdentifier(config.solidBlock);
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
        int spawnY = Voxel::findFirstAirY(*generator, config, spawnX, spawnZ);
        m_impl->camera.position.y = static_cast<float>(spawnY) + 0.5f;

        Render::initDebugField(m_impl->debug, m_impl->assets);
        Render::initFrameGraph(m_impl->debug, m_impl->assets);
        Render::initEntityDebug(m_impl->debug, m_impl->assets);
        m_impl->initTaa();
        m_impl->world.ready = true;
    } catch (const std::exception& e) {
        spdlog::error("Voxel bootstrap failed: {}", e.what());
    }
}

Application::~Application() {
    if (m_impl && m_impl->world.ready && m_impl->world.world) {
        try {
            Persistence::saveWorldToDisk(
                *m_impl->world.world,
                m_impl->world.worldSet.persistenceService(),
                m_impl->world.worldSet.persistenceContext(m_impl->world.activeWorldId));
        } catch (const std::exception& e) {
            spdlog::error("World save failed: {}", e.what());
        }
    }

    if (m_impl && m_impl->window.window) {
        glfwMakeContextCurrent(m_impl->window.window);

        UI::shutdown();

        Render::releaseDebugResources(m_impl->debug);
        if (m_impl->render.taa.quadVao != 0) {
            glDeleteVertexArrays(1, &m_impl->render.taa.quadVao);
            m_impl->render.taa.quadVao = 0;
        }
        m_impl->releaseTaaTargets();
        m_impl->render.taa.initialized = false;

        if (m_impl->world.worldView) {
            m_impl->world.worldView->setChunkLoader({});
            m_impl->world.worldView->setChunkPendingCallback({});
            m_impl->world.worldView->setChunkLoadDrain({});
            m_impl->world.worldView->setChunkLoadCancel({});
        }
        m_impl->world.chunkLoader.reset();

        if (m_impl->world.worldView) {
            m_impl->world.worldView->clear();
            m_impl->world.worldView->releaseRenderResources();
        }
        if (m_impl->world.world) {
            m_impl->world.world->clear();
        }
        m_impl->world.worldSet.resources().releaseRenderResources();
        m_impl->world.worldSet.clear();
        m_impl->world.worldView = nullptr;
        m_impl->world.world = nullptr;
        m_impl->assets.clearCache();

        glfwDestroyWindow(m_impl->window.window);
        m_impl->window.window = nullptr;
    }
    glfwTerminate();
    spdlog::info("Application terminated successfully");
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

        // Frame setup
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

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
                Render::recordFrameTime(m_impl->debug, deltaTime);
                Input::keyupdate();
                m_impl->input.dispatcher.update();
            }

            if (m_impl->world.ready && m_impl->world.world && m_impl->world.worldView) {
                if (m_impl->input.dispatcher.isActionJustPressed("toggle_mouse_capture")) {
                    Input::setCursorCaptured(m_impl->window, !m_impl->window.cursorCaptured);
                }
                if (m_impl->input.dispatcher.isActionJustPressed("debug_toggle_near_terrain")) {
                    const bool enabled = !m_impl->world.worldView->nearTerrainRenderingEnabled();
                    m_impl->world.worldView->setNearTerrainRenderingEnabled(enabled);
                    spdlog::info("Debug near terrain rendering: {}", enabled ? "enabled" : "disabled");
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
                float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

                const auto renderConfig = m_impl->world.worldView->renderConfig();
                float renderDistance = renderConfig.renderDistance;
                if (renderConfig.svoVoxel.enabled && renderConfig.svoVoxel.maxRadiusChunks > 0) {
                    const float svoDistance =
                        (static_cast<float>(renderConfig.svoVoxel.maxRadiusChunks) + 0.5f) *
                        static_cast<float>(Voxel::Chunk::SIZE);
                    renderDistance = std::max(renderDistance, svoDistance);
                }
                float nearPlane = 0.1f;
                float farPlane = std::max(500.0f, renderDistance + static_cast<float>(Voxel::Chunk::SIZE));
                glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, nearPlane, farPlane);
                glm::mat4 projectionNoJitter = projection;
                glm::mat4 view = glm::lookAt(
                    m_impl->camera.position,
                    m_impl->camera.target,
                    glm::vec3(0.0f, 1.0f, 0.0f)
                );

                bool useTaa = m_impl->world.worldView->renderConfig().taa.enabled;
                if (useTaa) {
                    m_impl->ensureTaaTargets(width, height);
                    useTaa = m_impl->render.taa.initialized && m_impl->render.taa.sceneFbo != 0;
                } else {
                    m_impl->render.taa.historyValid = false;
                }

                glm::vec2 jitter(0.0f);
                if (useTaa) {
                    jitter = m_impl->nextJitter(width, height,
                                                m_impl->world.worldView->renderConfig().taa.jitterScale);
                    projection[2][0] += jitter.x;
                    projection[2][1] += jitter.y;
                }

                glBindFramebuffer(GL_FRAMEBUFFER, useTaa ? m_impl->render.taa.sceneFbo : 0);
                glViewport(0, 0, width, height);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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
                }

                {
                    PROFILE_SCOPE("Render");
                    m_impl->world.worldView->render(view, projection, m_impl->camera.position,
                                                    nearPlane, farPlane, deltaTime);

                    if (useTaa) {
                        Render::renderEntityDebugBoxes(m_impl->debug, m_impl->world.world, view, projection);
                    }

                    if (useTaa) {
                        PROFILE_SCOPE("TAA");
                        glm::mat4 viewProjection = projection * view;
                        glm::mat4 viewProjectionNoJitter = projectionNoJitter * view;
                        glm::mat4 invViewProjection = glm::inverse(viewProjectionNoJitter);
                        glm::vec2 jitterUv = jitter * 0.5f;
                        m_impl->resolveTaa(invViewProjection,
                                           viewProjectionNoJitter,
                                           jitterUv,
                                           m_impl->world.worldView->renderConfig().taa.blend);
                        glViewport(0, 0, width, height);
                    }

                    if (!useTaa) {
                        Render::renderEntityDebugBoxes(m_impl->debug, m_impl->world.world,
                                                       view, projectionNoJitter);
                    }

                    Render::renderDebugField(m_impl->debug,
                                             m_impl->world.worldView,
                                             m_impl->camera.position,
                                             m_impl->camera.target,
                                             m_impl->camera.forward,
                                             width,
                                             height);
                    Render::renderFrameGraph(m_impl->debug);
#if defined(RIGEL_ENABLE_IMGUI)
                    UI::renderProfilerWindow(
                        m_impl->debug.imguiEnabled,
                        &m_impl->world.worldView->svoVoxelConfig(),
                        &m_impl->world.worldView->svoVoxelTelemetry()
                    );
#else
                    (void)width;
                    (void)height;
#endif
                }
            } else {
                int width = 0;
                int height = 0;
                glfwGetFramebufferSize(m_impl->window.window, &width, &height);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, width, height);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
