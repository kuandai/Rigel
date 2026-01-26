#include "Rigel/Application.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Entity/EntityModelLoader.h"
#include "Rigel/Persistence/Backends/CR/CRFormat.h"
#include "Rigel/Persistence/Backends/CR/CRSettings.h"
#include "Rigel/Persistence/Backends/Memory/MemoryFormat.h"
#include "Rigel/Persistence/ChunkSpanMerge.h"
#include "Rigel/Persistence/Storage.h"
#include "Rigel/Voxel/ChunkBenchmark.h"
#include "Rigel/Voxel/ChunkTasks.h"
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

        struct RegionKeyHash {
            size_t operator()(const Persistence::RegionKey& key) const {
                size_t seed = std::hash<std::string>{}(key.zoneId);
                size_t hx = std::hash<int32_t>{}(key.x);
                size_t hy = std::hash<int32_t>{}(key.y);
                size_t hz = std::hash<int32_t>{}(key.z);
                seed ^= hx + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                seed ^= hy + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                seed ^= hz + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        struct AsyncChunkLoader {
            struct LoadResult {
                Persistence::RegionKey key;
                Persistence::ChunkRegionSnapshot region;
                bool ok = false;
            };

            struct RegionEntry {
                Persistence::ChunkRegionSnapshot region;
                std::unordered_set<Voxel::ChunkCoord, Voxel::ChunkCoordHash> present;
            };

            Persistence::PersistenceService* service = nullptr;
            Persistence::PersistenceContext context;
            std::unique_ptr<Persistence::PersistenceFormat> format;
            Voxel::World* world = nullptr;
            uint32_t worldGenVersion = 0;
            std::string zoneId = "rigel:default";
            size_t maxCachedRegions = 8;
            size_t maxInFlightRegions = 8;
            int prefetchRadius = 1;
            std::shared_ptr<Voxel::WorldGenerator> generator;
            Voxel::detail::ThreadPool pool;
            Voxel::detail::ConcurrentQueue<LoadResult> completed;
            std::unordered_map<Persistence::RegionKey, RegionEntry, RegionKeyHash> cache;
            std::unordered_set<Persistence::RegionKey, RegionKeyHash> inFlight;
            std::deque<Persistence::RegionKey> lru;

            AsyncChunkLoader(Persistence::PersistenceService& serviceIn,
                             Persistence::PersistenceContext contextIn,
                             Voxel::World& worldIn,
                             uint32_t worldGenVersionIn,
                             size_t ioThreads,
                             int viewDistanceChunks,
                             std::shared_ptr<Voxel::WorldGenerator> generatorIn)
                : service(&serviceIn),
                  context(std::move(contextIn)),
                  world(&worldIn),
                  worldGenVersion(worldGenVersionIn),
                  generator(std::move(generatorIn)),
                  pool(ioThreads) {
                format = service->openFormat(context);
                int regionSpan = estimateRegionSpan();
                if (regionSpan < 1) {
                    regionSpan = 1;
                }
                int radius = viewDistanceChunks / regionSpan;
                if (radius < 1) {
                    radius = 1;
                }
                if (radius > 2) {
                    radius = 2;
                }
                prefetchRadius = radius;
            }

            bool load(Voxel::ChunkCoord coord) {
                if (!format || !world) {
                    return false;
                }

                drainCompletions();

                Persistence::RegionKey key =
                    format->regionLayout().regionForChunk(zoneId, coord);
                auto cacheIt = cache.find(key);
                if (cacheIt == cache.end()) {
                    queueRegionLoad(key);
                }
                prefetchNeighbors(key);
                if (cacheIt == cache.end()) {
                    return false;
                }

                if (cacheIt->second.present.find(coord) == cacheIt->second.present.end()) {
                    return false;
                }

                touch(key);
                return applyRegionChunk(cacheIt->second.region, coord);
            }

            bool isPending(Voxel::ChunkCoord coord) const {
                if (!format) {
                    return false;
                }
                Persistence::RegionKey key =
                    format->regionLayout().regionForChunk(zoneId, coord);
                return cache.find(key) == cache.end();
            }

        private:
            int estimateRegionSpan() const {
                if (!format) {
                    return 1;
                }
                Voxel::ChunkCoord origin{0, 0, 0};
                Persistence::RegionKey base =
                    format->regionLayout().regionForChunk(zoneId, origin);
                constexpr int kMaxSpan = 64;
                for (int offset = 1; offset <= kMaxSpan; ++offset) {
                    Voxel::ChunkCoord probe{offset, 0, 0};
                    Persistence::RegionKey key =
                        format->regionLayout().regionForChunk(zoneId, probe);
                    if (!(key == base)) {
                        return offset;
                    }
                }
                return kMaxSpan;
            }

            void drainCompletions() {
                LoadResult result;
                while (completed.tryPop(result)) {
                    inFlight.erase(result.key);
                    if (!result.ok) {
                        spdlog::warn("Region load failed ({} {} {}), treating as empty",
                                     result.key.x, result.key.y, result.key.z);
                        result.region = Persistence::ChunkRegionSnapshot{};
                        result.region.key = result.key;
                    }
                    applyRegionToLoadedChunks(result.region);
                    cache[result.key] = buildRegionEntry(std::move(result.region));
                    touch(result.key);
                    evictIfNeeded();
                }
            }

            void queueRegionLoad(const Persistence::RegionKey& key) {
                if (cache.find(key) != cache.end()) {
                    return;
                }
                if (inFlight.find(key) != inFlight.end()) {
                    return;
                }
                if (maxInFlightRegions > 0 && inFlight.size() >= maxInFlightRegions) {
                    return;
                }
                inFlight.insert(key);
                Persistence::PersistenceService* servicePtr = service;
                Persistence::PersistenceContext contextCopy = context;
                auto job = [this, servicePtr, contextCopy, key]() mutable {
                    LoadResult result;
                    result.key = key;
                    try {
                        auto jobFormat = servicePtr->openFormat(contextCopy);
                        result.region = jobFormat->chunkContainer().loadRegion(key);
                        result.ok = true;
                    } catch (const std::exception& e) {
                        spdlog::warn("Async region load failed ({} {} {}): {}",
                                     key.x, key.y, key.z, e.what());
                        result.ok = false;
                    }
                    completed.push(std::move(result));
                };

                if (pool.threadCount() > 0) {
                    pool.enqueue(std::move(job));
                } else {
                    job();
                }
            }

            bool applyRegionChunk(const Persistence::ChunkRegionSnapshot& region,
                                  Voxel::ChunkCoord coord) {
                if (region.chunks.empty()) {
                    return false;
                }

                std::vector<const Persistence::ChunkSnapshot*> matches;
                collectChunkSpans(region, coord, matches);

                if (matches.empty()) {
                    return false;
                }

                Voxel::Chunk& chunk = world->chunkManager().getOrCreateChunk(coord);
                chunk.setWorldGenVersion(worldGenVersion);

                Persistence::ChunkBaseFillFn baseFill;
                if (generator) {
                    baseFill = [this, coord](Voxel::Chunk& target,
                                             const Voxel::BlockRegistry& registry) {
                        Voxel::ChunkBuffer buffer;
                        generator->generate(coord, buffer, nullptr);
                        target.copyFrom(buffer.blocks, registry);
                        target.clearPersistDirty();
                    };
                }
                auto result = Persistence::mergeChunkSpans(
                    chunk,
                    world->blockRegistry(),
                    matches,
                    baseFill);

                chunk.clearDirty();
                chunk.clearPersistDirty();
                return result.loadedFromDisk;
            }

            RegionEntry buildRegionEntry(Persistence::ChunkRegionSnapshot region) {
                RegionEntry entry;
                entry.region = std::move(region);
                entry.present.reserve(entry.region.chunks.size());
                for (const auto& snapshot : entry.region.chunks) {
                    const Persistence::ChunkSpan& span = snapshot.data.span;
                    entry.present.insert(Voxel::ChunkCoord{span.chunkX, span.chunkY, span.chunkZ});
                }
                return entry;
            }

            void collectChunkSpans(const Persistence::ChunkRegionSnapshot& region,
                                   Voxel::ChunkCoord coord,
                                   std::vector<const Persistence::ChunkSnapshot*>& out) const {
                out.clear();
                out.reserve(8);
                for (const auto& snapshot : region.chunks) {
                    const Persistence::ChunkSpan& span = snapshot.data.span;
                    if (span.chunkX != coord.x || span.chunkY != coord.y || span.chunkZ != coord.z) {
                        continue;
                    }
                    out.push_back(&snapshot);
                }
            }

            void applyRegionToLoadedChunks(const Persistence::ChunkRegionSnapshot& region) {
                if (!world) {
                    return;
                }
                std::unordered_map<Voxel::ChunkCoord,
                                   std::vector<const Persistence::ChunkSnapshot*>,
                                   Voxel::ChunkCoordHash> grouped;
                grouped.reserve(region.chunks.size());

                for (const auto& snapshot : region.chunks) {
                    const Persistence::ChunkSpan& span = snapshot.data.span;
                    Voxel::ChunkCoord coord{span.chunkX, span.chunkY, span.chunkZ};
                    grouped[coord].push_back(&snapshot);
                }

                for (const auto& [coord, spans] : grouped) {
                    Voxel::Chunk* chunk = world->chunkManager().getChunk(coord);
                    if (!chunk) {
                        continue;
                    }
                    if (chunk->isPersistDirty()) {
                        continue;
                    }
                    Persistence::mergeChunkSpans(*chunk,
                                                 world->blockRegistry(),
                                                 spans,
                                                 {});
                    chunk->setWorldGenVersion(worldGenVersion);
                    chunk->clearPersistDirty();
                }
            }

            void prefetchNeighbors(const Persistence::RegionKey& center) {
                if (prefetchRadius <= 0) {
                    return;
                }
                for (int dz = -prefetchRadius; dz <= prefetchRadius; ++dz) {
                    for (int dy = -prefetchRadius; dy <= prefetchRadius; ++dy) {
                        for (int dx = -prefetchRadius; dx <= prefetchRadius; ++dx) {
                            if (dx == 0 && dy == 0 && dz == 0) {
                                continue;
                            }
                            Persistence::RegionKey neighbor = center;
                            neighbor.x += dx;
                            neighbor.y += dy;
                            neighbor.z += dz;
                            queueRegionLoad(neighbor);
                        }
                    }
                }
            }

            void touch(const Persistence::RegionKey& key) {
                auto it = std::find(lru.begin(), lru.end(), key);
                if (it != lru.end()) {
                    lru.erase(it);
                }
                lru.push_back(key);
            }

            void evictIfNeeded() {
                if (maxCachedRegions == 0) {
                    return;
                }
                while (cache.size() > maxCachedRegions && !lru.empty()) {
                    Persistence::RegionKey key = lru.front();
                    lru.pop_front();
                    cache.erase(key);
                }
            }
        };

        uint32_t worldGenVersion = generator->config().world.version;
        size_t ioThreads = 1;
        auto chunkLoader = std::make_shared<AsyncChunkLoader>(
            m_impl->world.worldSet.persistenceService(),
            std::move(persistenceContext),
            *m_impl->world.world,
            worldGenVersion,
            ioThreads,
            config.stream.viewDistanceChunks,
            generator);
        m_impl->world.worldView->setChunkLoader(
            [chunkLoader](Voxel::ChunkCoord coord) {
                return chunkLoader->load(coord);
            });
        m_impl->world.worldView->setChunkPendingCallback(
            [chunkLoader](Voxel::ChunkCoord coord) {
                return chunkLoader->isPending(coord);
            });

        Voxel::ConfigProvider renderConfigProvider =
            Voxel::makeRenderConfigProvider(m_impl->assets, m_impl->world.activeWorldId);
        Voxel::WorldRenderConfig renderConfig = renderConfigProvider.loadRenderConfig();
        const char* profileEnv = std::getenv("RIGEL_PROFILE");
        if (profileEnv && profileEnv[0] != '\0') {
            renderConfig.profilingEnabled = (profileEnv[0] != '0');
        }
        m_impl->world.worldView->renderConfig() = renderConfig;
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

                float renderDistance = m_impl->world.worldView->renderConfig().renderDistance;
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
                    m_impl->world.worldView->updateStreaming(m_impl->camera.position);
                    m_impl->world.worldView->updateMeshes();
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
                    UI::renderProfilerWindow(m_impl->debug.overlayEnabled);
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

        // Exit on ESC
        if (m_impl->input.dispatcher.isActionPressed("exit")) {
            glfwSetWindowShouldClose(m_impl->window.window, true);
        }
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
