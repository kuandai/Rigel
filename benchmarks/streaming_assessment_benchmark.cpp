#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Config/ConfigSource.h"
#include "Rigel/Render/DebugOverlay.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Render/OpenGLRuntime.h"
#include "Rigel/UI/ImGuiLayer.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldConfigProvider.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldMeshStore.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef RIGEL_BENCHMARK_BUILD_TYPE
#define RIGEL_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using namespace Rigel;
using Clock = std::chrono::steady_clock;

struct Percentiles {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

Percentiles summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto nearestRank = [&](double percentile) {
        const size_t rank = static_cast<size_t>(
            std::ceil(percentile * static_cast<double>(values.size())));
        return values[std::clamp<size_t>(rank, 1, values.size()) - 1];
    };
    return {nearestRank(0.50), nearestRank(0.95), nearestRank(0.99)};
}

void printPercentiles(std::string_view prefix, const Percentiles& values) {
    std::cout << ' ' << prefix << "_p50_ms=" << values.p50
              << ' ' << prefix << "_p95_ms=" << values.p95
              << ' ' << prefix << "_p99_ms=" << values.p99;
}

Voxel::WorldConfiguration loadShippedWorldConfiguration(
    Asset::AssetManager& assets) {
    Voxel::WorldConfigProvider provider;
    provider.addSource(std::make_unique<Config::EmbeddedConfigSource>(
        assets, "raw/world_config"));
    return provider.loadConfig();
}

void registerGenerationBlocks(Voxel::BlockRegistry& registry,
                              const Voxel::WorldGenConfig& config) {
    std::set<std::string> identifiers{
        config.solidBlock,
        config.surfaceBlock,
        "base:water[type=source]",
        "base:sand"};
    for (const auto& biome : config.biomes.entries) {
        for (const auto& layer : biome.surface) {
            identifiers.insert(layer.block);
        }
    }
    for (const auto& feature : config.structures.features) {
        identifiers.insert(feature.block);
    }
    for (const std::string& identifier : identifiers) {
        Voxel::BlockType block;
        block.identifier = identifier;
        block.isOpaque = identifier != "base:water[type=source]";
        block.isSolid = block.isOpaque;
        registry.registerBlock(identifier, std::move(block));
    }
}

size_t nonAirBlocks(const Voxel::ChunkBuffer& buffer) {
    return static_cast<size_t>(std::count_if(
        buffer.blocks.begin(), buffer.blocks.end(),
        [](const Voxel::BlockState& block) { return !block.isAir(); }));
}

struct LifecycleResult {
    bool quiescent = false;
    size_t targetNonAirBlocks = 0;
    Voxel::ChunkStreamer::DebugState targetState =
        Voxel::ChunkStreamer::DebugState::WaitingForData;
    Voxel::ChunkStreamer::DebugInstalledGeometry targetGeometry =
        Voxel::ChunkStreamer::DebugInstalledGeometry::None;
    Voxel::ChunkStreamer::WorkMetrics work;
    uint64_t updates = 0;
};

LifecycleResult runOutOfBoundsLifecycle(
    Voxel::ChunkCoord target,
    const Voxel::StreamingConfig& shippedStreaming,
    Voxel::BlockRegistry& registry,
    const std::shared_ptr<const Voxel::WorldGenerator>& generator) {
    Voxel::ChunkManager manager;
    manager.setRegistry(&registry);
    Voxel::WorldMeshStore meshStore;
    Voxel::ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);
    Voxel::StreamingConfig config = shippedStreaming;
    config.viewDistanceChunks = 1;
    config.unloadDistanceChunks = 1;
    streamer.setConfig(config);
    streamer.setChunkLoader([](Voxel::ChunkLoadRequest) {
        return Voxel::ChunkLoadRequestResult::Missing;
    });
    streamer.markSpawnDiscoveryComplete();

    LifecycleResult result;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline) {
        streamer.update(target.toWorldCenter());
        streamer.processCompletions();
        ++result.updates;
        if (streamer.diagnostics().state ==
            Voxel::StreamingLifecycleState::Quiescent) {
            result.quiescent = true;
            break;
        }
        std::this_thread::yield();
    }

    result.work = streamer.workMetrics();
    if (const Voxel::Chunk* chunk = manager.getChunk(target)) {
        result.targetNonAirBlocks = chunk->nonAirCount();
    }
    std::vector<Voxel::ChunkStreamer::DebugChunkState> states;
    streamer.getDebugStates(states, target, 0);
    if (!states.empty()) {
        result.targetState = states.front().state;
        result.targetGeometry = states.front().installedGeometry;
    }
    return result;
}

const char* debugStateName(Voxel::ChunkStreamer::DebugState state) {
    using State = Voxel::ChunkStreamer::DebugState;
    switch (state) {
        case State::WaitingForData: return "waiting_for_data";
        case State::WaitingForNeighbors: return "waiting_for_neighbors";
        case State::MeshSchedulerWait: return "mesh_scheduler_wait";
        case State::MeshSubmittedOrBuilding: return "mesh_work";
        case State::VoxelEmpty: return "voxel_empty";
        case State::AcceptedEmptyGeometry: return "accepted_empty_geometry";
        case State::AcceptedNonemptyGeometry:
            return "accepted_nonempty_geometry";
        case State::DirtyRemeshPending: return "dirty_remesh_pending";
        case State::TerminalFailure: return "terminal_failure";
        case State::Count: break;
    }
    return "unknown";
}

bool runVerticalAssessment(Asset::AssetManager& assets) {
    const auto configuration = loadShippedWorldConfiguration(assets);
    Voxel::BlockRegistry registry;
    registerGenerationBlocks(registry, configuration.generation);
    const auto generator = std::make_shared<const Voxel::WorldGenerator>(
        registry, configuration.generation);

    const int belowY = Voxel::worldToChunk(
        0, configuration.generation.world.minY, 0).y - 1;
    const int aboveY = Voxel::worldToChunk(
        0, configuration.generation.world.maxY, 0).y + 1;
    const std::array<std::pair<int, int>, 9> columns{{
        {0, 0}, {4, 0}, {-4, 0}, {0, 4}, {0, -4},
        {8, 8}, {-8, 8}, {8, -8}, {-8, -8}}};

    for (const auto& [label, chunkY] :
         std::array<std::pair<std::string_view, int>, 2>{{
             {"below", belowY}, {"above", aboveY}}}) {
        std::vector<double> generationTimes;
        size_t nonemptyChunks = 0;
        size_t nonAirTotal = 0;
        for (const auto& [x, z] : columns) {
            Voxel::ChunkBuffer buffer;
            const auto start = Clock::now();
            generator->generate({x, chunkY, z}, buffer);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                Clock::now() - start).count();
            generationTimes.push_back(elapsed);
            const size_t occupancy = nonAirBlocks(buffer);
            nonAirTotal += occupancy;
            nonemptyChunks += occupancy != 0 ? 1 : 0;
        }
        std::cout << "vertical_generation position=" << label
                  << " chunk_y=" << chunkY
                  << " samples=" << columns.size()
                  << " nonempty_chunks=" << nonemptyChunks
                  << " total_non_air_blocks=" << nonAirTotal;
        printPercentiles("execution", summarize(std::move(generationTimes)));
        std::cout << '\n';

        const auto lifecycle = runOutOfBoundsLifecycle(
            {0, chunkY, 0},
            configuration.streaming,
            registry,
            generator);
        std::cout << "vertical_lifecycle position=" << label
                  << " radius=1"
                  << " shipped_worker_threads="
                  << configuration.streaming.workerThreads
                  << " generation_workers="
                  << configuration.streaming.workerThreads -
                         configuration.streaming.workerThreads / 2
                  << " mesh_workers="
                  << configuration.streaming.workerThreads / 2
                  << " target_non_air_blocks="
                  << lifecycle.targetNonAirBlocks
                  << " target_state="
                  << debugStateName(lifecycle.targetState)
                  << " target_geometry="
                  << (lifecycle.targetGeometry ==
                              Voxel::ChunkStreamer::DebugInstalledGeometry::Nonempty
                          ? "nonempty"
                          : lifecycle.targetGeometry ==
                                    Voxel::ChunkStreamer::DebugInstalledGeometry::Empty
                              ? "empty"
                              : "none")
                  << " generation_started="
                  << lifecycle.work.generationJobsStarted
                  << " generation_completed="
                  << lifecycle.work.generationJobsCompleted
                  << " mesh_started=" << lifecycle.work.meshJobsStarted
                  << " mesh_accepted=" << lifecycle.work.meshJobsAccepted
                  << " updates=" << lifecycle.updates
                  << " completion_state="
                  << (lifecycle.quiescent ? "quiescent" : "timeout")
                  << '\n';
        if (!lifecycle.quiescent) {
            return false;
        }
    }
    return true;
}

class OpenGLContext {
public:
    OpenGLContext() {
        if (!glfwInit()) {
            const char* description = nullptr;
            glfwGetError(&description);
            m_failure = description
                ? std::string("glfw_initialization: ") + description
                : "glfw_initialization";
            return;
        }
        m_glfwInitialized = true;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,
                       Render::kOpenGLContextMajorVersion);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,
                       Render::kOpenGLContextMinorVersion);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        m_window = glfwCreateWindow(800, 600, "Rigel overlay benchmark",
                                    nullptr, nullptr);
        if (!m_window) {
            const char* description = nullptr;
            glfwGetError(&description);
            m_failure = description
                ? std::string("window_creation: ") + description
                : "window_creation";
            return;
        }
        glfwMakeContextCurrent(m_window);
        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        if (glewStatus != GLEW_OK) {
            m_failure = "glew_initialization: ";
            m_failure += reinterpret_cast<const char*>(
                glewGetErrorString(glewStatus));
            return;
        }
        glGetError();
        m_ready = UI::init(m_window);
        if (!m_ready) {
            m_failure = "imgui_initialization";
        }
    }

    ~OpenGLContext() {
        UI::shutdown();
        if (m_window) {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(m_window);
        }
        if (m_glfwInitialized) {
            glfwTerminate();
        }
    }

    bool ready() const { return m_ready; }
    const std::string& failure() const { return m_failure; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
    bool m_ready = false;
    std::string m_failure;
};

void GLAPIENTRY ignoreUseProgram(GLuint) {}
void GLAPIENTRY ignoreUniformMatrix4fv(
    GLint, GLsizei, GLboolean, const GLfloat*) {}
void GLAPIENTRY ignoreUniform3fv(GLint, GLsizei, const GLfloat*) {}
void GLAPIENTRY ignoreUniform1f(GLint, GLfloat) {}
void GLAPIENTRY ignoreUniform4fv(GLint, GLsizei, const GLfloat*) {}
void GLAPIENTRY ignoreBindVertexArray(GLuint) {}
void GLAPIENTRY ignoreBindBuffer(GLenum, GLuint) {}
void GLAPIENTRY ignoreBufferData(GLenum, GLsizeiptr, const void*, GLenum) {}
void GLAPIENTRY ignoreEnableVertexAttribArray(GLuint) {}
void GLAPIENTRY ignoreVertexAttribPointer(
    GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}

class ScopedOverlayGlCalls {
public:
    ScopedOverlayGlCalls()
        : m_useProgram(__glewUseProgram)
        , m_uniformMatrix4fv(__glewUniformMatrix4fv)
        , m_uniform3fv(__glewUniform3fv)
        , m_uniform1f(__glewUniform1f)
        , m_uniform4fv(__glewUniform4fv)
        , m_bindVertexArray(__glewBindVertexArray)
        , m_bindBuffer(__glewBindBuffer)
        , m_bufferData(__glewBufferData)
        , m_enableVertexAttribArray(__glewEnableVertexAttribArray)
        , m_vertexAttribPointer(__glewVertexAttribPointer) {
        __glewUseProgram = &ignoreUseProgram;
        __glewUniformMatrix4fv = &ignoreUniformMatrix4fv;
        __glewUniform3fv = &ignoreUniform3fv;
        __glewUniform1f = &ignoreUniform1f;
        __glewUniform4fv = &ignoreUniform4fv;
        __glewBindVertexArray = &ignoreBindVertexArray;
        __glewBindBuffer = &ignoreBindBuffer;
        __glewBufferData = &ignoreBufferData;
        __glewEnableVertexAttribArray = &ignoreEnableVertexAttribArray;
        __glewVertexAttribPointer = &ignoreVertexAttribPointer;
    }

    ~ScopedOverlayGlCalls() {
        __glewUseProgram = m_useProgram;
        __glewUniformMatrix4fv = m_uniformMatrix4fv;
        __glewUniform3fv = m_uniform3fv;
        __glewUniform1f = m_uniform1f;
        __glewUniform4fv = m_uniform4fv;
        __glewBindVertexArray = m_bindVertexArray;
        __glewBindBuffer = m_bindBuffer;
        __glewBufferData = m_bufferData;
        __glewEnableVertexAttribArray = m_enableVertexAttribArray;
        __glewVertexAttribPointer = m_vertexAttribPointer;
    }

private:
    PFNGLUSEPROGRAMPROC m_useProgram;
    PFNGLUNIFORMMATRIX4FVPROC m_uniformMatrix4fv;
    PFNGLUNIFORM3FVPROC m_uniform3fv;
    PFNGLUNIFORM1FPROC m_uniform1f;
    PFNGLUNIFORM4FVPROC m_uniform4fv;
    PFNGLBINDVERTEXARRAYPROC m_bindVertexArray;
    PFNGLBINDBUFFERPROC m_bindBuffer;
    PFNGLBUFFERDATAPROC m_bufferData;
    PFNGLENABLEVERTEXATTRIBARRAYPROC m_enableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC m_vertexAttribPointer;
};

bool prepareOverlayStreaming(Voxel::WorldView& view,
                             const glm::vec3& cameraPosition) {
    const Voxel::ChunkCoord center = Voxel::worldToChunk(
        static_cast<int>(std::floor(cameraPosition.x)),
        static_cast<int>(std::floor(cameraPosition.y)),
        static_cast<int>(std::floor(cameraPosition.z)));
    view.setChunkLoader([center](Voxel::ChunkLoadRequest request) {
        const auto squared = [](int value) {
            const int64_t widened = value;
            return static_cast<uint64_t>(widened * widened);
        };
        const uint64_t distanceSquared =
            squared(request.coord.x - center.x) +
            squared(request.coord.y - center.y) +
            squared(request.coord.z - center.z);
        return distanceSquared <= 1
            ? Voxel::ChunkLoadRequestResult::Missing
            : Voxel::ChunkLoadRequestResult::Queued;
    });
    view.markSpawnDiscoveryComplete();
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    std::vector<Voxel::ChunkStreamer::DebugChunkState> targetState;
    while (Clock::now() < deadline) {
        view.updateStreaming(cameraPosition);
        view.updateMeshes();
        view.getChunkDebugStates(targetState, center, 0);
        if (!targetState.empty() &&
            targetState.front().installedGeometry ==
                Voxel::ChunkStreamer::DebugInstalledGeometry::Nonempty) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool runOverlayCpuAssessment(Asset::AssetManager& assets, size_t frames) {
    const auto configuration = loadShippedWorldConfiguration(assets);
    Voxel::WorldResources resources;
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    registerGenerationBlocks(resources.registry(), configuration.generation);
    const auto generator = std::make_shared<const Voxel::WorldGenerator>(
        resources.registry(), configuration.generation);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.setStreamConfig(configuration.streaming);
    const glm::vec3 cameraPosition(16.0f, 80.0f, 16.0f);
    if (!prepareOverlayStreaming(view, cameraPosition)) {
        std::cerr << "overlay_cpu_assessment_failed "
                     "reason=nonempty_target_geometry_unavailable\n";
        return false;
    }

    auto shader = std::make_shared<Asset::ShaderAsset>();
    shader->program = 1;
    Render::DebugState debug;
    debug.field.initialized = true;
    debug.field.vao = 1;
    debug.field.vbos.fill(1);
    debug.field.shader = Asset::Handle<Asset::ShaderAsset>(
        shader, "benchmark/chunk_debug");
    const bool startupEnabled = debug.overlayEnabled;
    const auto renderFrame = [&](bool enabled) {
        debug.overlayEnabled = enabled;
        const auto start = Clock::now();
        Render::renderDebugField(
            debug,
            &view,
            cameraPosition,
            glm::vec3(16.0f, 80.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, -1.0f),
            800,
            600);
        return std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
    };

    ScopedOverlayGlCalls glCalls;
    renderFrame(false);
    renderFrame(true);
    const size_t trackedRecords = debug.debugStates.size();
    const size_t drawEvidenceRecords = static_cast<size_t>(std::count_if(
        debug.debugStates.begin(), debug.debugStates.end(),
        [](const auto& state) {
            return state.drawEvidence !=
                Voxel::ChunkStreamer::DebugDrawEvidence::NotApplicable;
        }));
    std::vector<double> disabled;
    std::vector<double> enabled;
    disabled.reserve(frames);
    enabled.reserve(frames);
    for (size_t index = 0; index < frames; ++index) {
        disabled.push_back(renderFrame(false));
        enabled.push_back(renderFrame(true));
    }
    const auto disabledSummary = summarize(std::move(disabled));
    const auto enabledSummary = summarize(std::move(enabled));
    const uint64_t diameter = static_cast<uint64_t>(
        configuration.streaming.viewDistanceChunks * 2 + 1);
    std::cout << "overlay_cpu_comparison frames_per_mode=" << frames
              << " view_radius="
              << configuration.streaming.viewDistanceChunks
              << " scanned_coordinates="
              << diameter * diameter * diameter
              << " tracked_records=" << trackedRecords
              << " draw_evidence_records=" << drawEvidenceRecords
              << " startup_overlay_enabled="
              << (startupEnabled ? "true" : "false")
              << " world_view_draw_evidence_decoration=true"
              << " presentation_maps_and_meshes=true"
              << " gl_calls=stubbed"
              << " imgui_legend_rendering=false";
    printPercentiles("disabled_frame", disabledSummary);
    printPercentiles("enabled_frame", enabledSummary);
    std::cout << " p95_delta_ms="
              << enabledSummary.p95 - disabledSummary.p95 << '\n';
    debug.field.shader = {};
    shader->program = 0;
    return true;
}

bool runOverlayAssessment(Asset::AssetManager& assets, size_t frames) {
    OpenGLContext context;
    if (!context.ready()) {
        std::cerr << "overlay_assessment_failed reason=\""
                  << context.failure() << "\"\n";
        return false;
    }

    const auto configuration = loadShippedWorldConfiguration(assets);
    Voxel::WorldResources resources;
    resources.initialize(assets);
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    const auto generator = std::make_shared<const Voxel::WorldGenerator>(
        resources.registry(), configuration.generation);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.initialize(assets);
    view.setStreamConfig(configuration.streaming);
    const glm::vec3 cameraPosition(16.0f, 80.0f, 16.0f);
    if (!prepareOverlayStreaming(view, cameraPosition)) {
        std::cerr << "overlay_assessment_failed "
                     "reason=nonempty_target_geometry_unavailable\n";
        return false;
    }

    Render::FrameRenderer renderer;
    renderer.initialize(assets);
    const bool startupEnabled = renderer.debugOverlayEnabled();
    std::vector<Voxel::ChunkStreamer::DebugChunkState> states;
    view.getChunkDebugStates(states, {0, 2, 0},
                             configuration.streaming.viewDistanceChunks);
    const size_t drawEvidenceRecords = static_cast<size_t>(std::count_if(
        states.begin(), states.end(),
        [](const auto& state) {
            return state.drawEvidence !=
                Voxel::ChunkStreamer::DebugDrawEvidence::NotApplicable;
        }));
    const Render::FrameRenderContext frame{
        world,
        view,
        cameraPosition,
        glm::vec3(16.0f, 80.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
        800,
        600,
        1.0f / 60.0f};

    const auto renderFrame = [&](bool enabled) {
        renderer.debugOverlayEnabled() = enabled;
        glFinish();
        const auto start = Clock::now();
        UI::beginFrame();
        renderer.recordFrameTime(frame.deltaTime);
        renderer.render(frame);
        UI::renderProfilerWindow(renderer.profilerWindowEnabled());
        UI::renderChunkDebugLegend(
            renderer.debugOverlayEnabled(), renderer.chunkDebugDetail());
        UI::endFrame();
        glFinish();
        return std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
    };

    renderFrame(false);
    renderFrame(true);
    std::vector<double> disabled;
    std::vector<double> enabled;
    disabled.reserve(frames);
    enabled.reserve(frames);
    for (size_t index = 0; index < frames; ++index) {
        if (index % 2 == 0) {
            disabled.push_back(renderFrame(false));
            enabled.push_back(renderFrame(true));
        } else {
            enabled.push_back(renderFrame(true));
            disabled.push_back(renderFrame(false));
        }
    }

    const auto disabledSummary = summarize(std::move(disabled));
    const auto enabledSummary = summarize(std::move(enabled));
    std::cout << "overlay_comparison frames_per_mode=" << frames
              << " view_radius="
              << configuration.streaming.viewDistanceChunks
              << " scanned_coordinates="
              << static_cast<uint64_t>(
                     configuration.streaming.viewDistanceChunks * 2 + 1) *
                     static_cast<uint64_t>(
                         configuration.streaming.viewDistanceChunks * 2 + 1) *
                     static_cast<uint64_t>(
                         configuration.streaming.viewDistanceChunks * 2 + 1)
              << " tracked_records=" << states.size()
              << " draw_evidence_records=" << drawEvidenceRecords
              << " startup_overlay_enabled="
              << (startupEnabled ? "true" : "false")
              << " draw_evidence_decoration=true"
              << " gl_field_rendering=true"
              << " imgui_legend_rendering=true";
    printPercentiles("disabled_frame", disabledSummary);
    printPercentiles("enabled_frame", enabledSummary);
    std::cout << " p95_delta_ms="
              << enabledSummary.p95 - disabledSummary.p95 << '\n';

    renderer.release();
    view.releaseRenderResources();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    size_t frames = 120;
    bool vertical = true;
    bool overlay = true;
    bool overlayCpu = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--vertical-only") {
            vertical = true;
            overlay = false;
        } else if (argument == "--overlay-only") {
            vertical = false;
            overlay = true;
        } else if (argument == "--overlay-cpu-only") {
            vertical = false;
            overlay = false;
            overlayCpu = true;
        } else if (argument == "--frames" && index + 1 < argc) {
            const long parsed = std::strtol(argv[++index], nullptr, 10);
            if (parsed <= 0) {
                std::cerr << "Invalid positive frame count\n";
                return 2;
            }
            frames = static_cast<size_t>(parsed);
        } else {
            std::cerr << "Unknown or incomplete option: " << argument << '\n';
            return 2;
        }
    }

    std::cout << std::fixed << std::setprecision(3)
              << "benchmark name=streaming_assessment version=1"
              << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " shipped_world_configuration=true\n";
    Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    if (vertical && !runVerticalAssessment(assets)) {
        return 1;
    }
    if (overlay && !runOverlayAssessment(assets, frames)) {
        return 1;
    }
    if (overlayCpu && !runOverlayCpuAssessment(assets, frames)) {
        return 1;
    }
    return 0;
}
