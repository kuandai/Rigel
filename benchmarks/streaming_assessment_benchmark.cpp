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
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef RIGEL_BENCHMARK_BUILD_TYPE
#define RIGEL_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using namespace Rigel;
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxFramesPerMode = 10000;

template <typename Callback>
class ScopeExit {
public:
    explicit ScopeExit(Callback callback)
        : m_callback(std::move(callback)) {}

    ~ScopeExit() noexcept {
        try {
            m_callback();
        } catch (...) {
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Callback m_callback;
};

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
    Voxel::StreamingDiagnosticSnapshot diagnostics;
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
    result.diagnostics = streamer.diagnostics();
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

using DebugStateCounts = std::array<
    size_t,
    static_cast<size_t>(Voxel::ChunkStreamer::DebugState::Count)>;

struct OverlayStartupSnapshot {
    Voxel::StreamingDiagnosticSnapshot diagnostics;
    Voxel::ChunkStreamer::WorkMetrics work;
    DebugStateCounts states{};
    size_t trackedRecords = 0;
};

bool overlayExecutionSettled(
    const Voxel::StreamingDiagnosticSnapshot& diagnostics,
    const Voxel::ChunkStreamer::WorkMetrics& work) {
    return diagnostics.generation.pending == 0 &&
        diagnostics.generation.inFlight == 0 &&
        diagnostics.generation.terminalErrors == 0 &&
        diagnostics.generationCompletionsPending == 0 &&
        diagnostics.sourceResolutionPending > 0 &&
        diagnostics.generationSchedulerPending == 0 &&
        diagnostics.retiredWorkPending == 0 &&
        diagnostics.chunkLoad.pending > 0 &&
        diagnostics.chunkLoad.inFlight == 0 &&
        diagnostics.chunkLoad.terminalErrors == 0 &&
        diagnostics.mesh.pending > 0 &&
        diagnostics.mesh.inFlight == 0 &&
        diagnostics.meshCompletionsPending == 0 &&
        diagnostics.mesh.terminalErrors == 0 &&
        diagnostics.eviction.pending == 0 &&
        diagnostics.eviction.inFlight == 0 &&
        diagnostics.eviction.terminalErrors == 0 &&
        work.generationJobsStarted > 0 &&
        work.generationJobsStarted == work.generationJobsCompleted &&
        work.generationJobsCancelled == 0 &&
        work.generationJobsFailed == 0 &&
        work.meshJobsStarted > 0 &&
        work.meshJobsStarted == work.meshJobsCompleted &&
        work.meshJobsCompleted == work.meshJobsAccepted +
            work.meshJobsRejectedStale + work.meshJobsFailed &&
        work.meshJobsFailed == 0;
}

OverlayStartupSnapshot captureOverlayStartup(
    Voxel::WorldView& view,
    Voxel::ChunkCoord center,
    int radius) {
    OverlayStartupSnapshot snapshot;
    snapshot.diagnostics = view.streamingDiagnostics();
    snapshot.work = view.streamingMetrics();
    std::vector<Voxel::ChunkStreamer::DebugChunkState> states;
    view.getChunkDebugStates(states, center, radius);
    snapshot.trackedRecords = states.size();
    for (const auto& state : states) {
        ++snapshot.states[static_cast<size_t>(state.state)];
    }
    return snapshot;
}

bool overlayStartupClassified(const OverlayStartupSnapshot& startup) {
    using State = Voxel::ChunkStreamer::DebugState;
    const auto count = [&](State state) {
        return startup.states[static_cast<size_t>(state)];
    };
    return overlayExecutionSettled(startup.diagnostics, startup.work) &&
        count(State::WaitingForData) ==
            startup.diagnostics.chunkLoad.pending &&
        count(State::WaitingForNeighbors) ==
            startup.diagnostics.mesh.pending &&
        count(State::MeshSchedulerWait) == 0 &&
        count(State::MeshSubmittedOrBuilding) == 0 &&
        count(State::DirtyRemeshPending) == 0 &&
        count(State::TerminalFailure) == 0;
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
                  << " generation_cancelled="
                  << lifecycle.work.generationJobsCancelled
                  << " generation_failed="
                  << lifecycle.work.generationJobsFailed
                  << " generation_pending="
                  << lifecycle.diagnostics.generation.pending
                  << " generation_in_flight="
                  << lifecycle.diagnostics.generation.inFlight
                  << " generation_completion_pending="
                  << lifecycle.diagnostics.generationCompletionsPending
                  << " generation_terminal_failures="
                  << lifecycle.diagnostics.generation.terminalErrors
                  << " canonical_source_work="
                  << lifecycle.diagnostics.sourceResolutionPending
                  << " canonical_generation_work="
                  << lifecycle.diagnostics.generationSchedulerPending
                  << " canonical_retired_work="
                  << lifecycle.diagnostics.retiredWorkPending
                  << " mesh_started=" << lifecycle.work.meshJobsStarted
                  << " mesh_completed=" << lifecycle.work.meshJobsCompleted
                  << " mesh_accepted=" << lifecycle.work.meshJobsAccepted
                  << " mesh_stale="
                  << lifecycle.work.meshJobsRejectedStale
                  << " mesh_failed=" << lifecycle.work.meshJobsFailed
                  << " mesh_pending="
                  << lifecycle.diagnostics.mesh.pending
                  << " mesh_in_flight="
                  << lifecycle.diagnostics.mesh.inFlight
                  << " mesh_completion_pending="
                  << lifecycle.diagnostics.meshCompletionsPending
                  << " mesh_terminal_failures="
                  << lifecycle.diagnostics.mesh.terminalErrors
                  << " chunk_load_pending="
                  << lifecycle.diagnostics.chunkLoad.pending
                  << " chunk_load_in_flight="
                  << lifecycle.diagnostics.chunkLoad.inFlight
                  << " chunk_load_terminal_failures="
                  << lifecycle.diagnostics.chunkLoad.terminalErrors
                  << " eviction_pending="
                  << lifecycle.diagnostics.eviction.pending
                  << " eviction_in_flight="
                  << lifecycle.diagnostics.eviction.inFlight
                  << " eviction_terminal_failures="
                  << lifecycle.diagnostics.eviction.terminalErrors
                  << " updates=" << lifecycle.updates
                  << " completion_state="
                  << (lifecycle.quiescent ? "quiescent" : "timeout")
                  << '\n';
        if (!lifecycle.quiescent || !lifecycle.diagnostics.workEmpty() ||
            lifecycle.diagnostics.sourceResolutionPending != 0 ||
            lifecycle.diagnostics.generationSchedulerPending != 0 ||
            lifecycle.diagnostics.generationCompletionsPending != 0 ||
            lifecycle.diagnostics.meshCompletionsPending != 0 ||
            lifecycle.diagnostics.retiredWorkPending != 0) {
            return false;
        }
    }
    return true;
}

class OpenGLContext {
public:
    OpenGLContext() {
        try {
            initialize();
        } catch (...) {
            release();
            throw;
        }
    }

    ~OpenGLContext() { release(); }

    bool ready() const { return m_ready; }
    const std::string& failure() const { return m_failure; }
    const std::string& renderer() const { return m_renderer; }
    const std::string& version() const { return m_version; }

private:
    void initialize() {
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
        const auto stringValue = [](GLenum name) {
            const GLubyte* value = glGetString(name);
            return value
                ? std::string(reinterpret_cast<const char*>(value))
                : std::string("unavailable");
        };
        m_renderer = stringValue(GL_RENDERER);
        m_version = stringValue(GL_VERSION);
        m_ready = UI::init(m_window);
        if (!m_ready) {
            m_failure = "imgui_initialization";
        }
    }

    void release() noexcept {
        try {
            if (m_window) {
                glfwMakeContextCurrent(m_window);
            }
            UI::shutdown();
        } catch (...) {
        }
        m_ready = false;
        if (m_window) {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }
        if (m_glfwInitialized) {
            glfwTerminate();
            m_glfwInitialized = false;
        }
    }

    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
    bool m_ready = false;
    std::string m_failure;
    std::string m_renderer = "unavailable";
    std::string m_version = "unavailable";
};

Voxel::ChunkCoord cameraChunk(const glm::vec3& cameraPosition) {
    return Voxel::worldToChunk(
        static_cast<int>(std::floor(cameraPosition.x)),
        static_cast<int>(std::floor(cameraPosition.y)),
        static_cast<int>(std::floor(cameraPosition.z)));
}

enum class OverlayPreparationResult : uint8_t {
    Ready,
    NonemptyTargetUnavailable,
    ExecutionNotSettled
};

OverlayPreparationResult prepareOverlayStreaming(
    Voxel::WorldView& view,
    const glm::vec3& cameraPosition) {
    const Voxel::ChunkCoord center = cameraChunk(cameraPosition);
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
    bool observedNonemptyTarget = false;
    while (Clock::now() < deadline) {
        view.updateStreaming(cameraPosition);
        view.updateMeshes();
        view.getChunkDebugStates(targetState, center, 0);
        if (!targetState.empty() &&
            targetState.front().installedGeometry ==
                Voxel::ChunkStreamer::DebugInstalledGeometry::Nonempty) {
            observedNonemptyTarget = true;
            if (overlayExecutionSettled(
                    view.streamingDiagnostics(), view.streamingMetrics())) {
                return OverlayPreparationResult::Ready;
            }
        }
        std::this_thread::yield();
    }
    return observedNonemptyTarget
        ? OverlayPreparationResult::ExecutionNotSettled
        : OverlayPreparationResult::NonemptyTargetUnavailable;
}

const char* overlayPreparationFailure(OverlayPreparationResult result) {
    return result == OverlayPreparationResult::ExecutionNotSettled
        ? "startup_execution_not_settled"
        : "nonempty_target_geometry_unavailable";
}

void printUnsettledExecution(
    const Voxel::StreamingDiagnosticSnapshot& diagnostics,
    const Voxel::ChunkStreamer::WorkMetrics& work) {
    std::cerr
        << " generation=" << diagnostics.generation.pending << '/'
        << diagnostics.generation.inFlight << '/'
        << diagnostics.generationCompletionsPending << '/'
        << diagnostics.generation.terminalErrors
        << " canonical=" << diagnostics.sourceResolutionPending << '/'
        << diagnostics.generationSchedulerPending << '/'
        << diagnostics.retiredWorkPending
        << " load=" << diagnostics.chunkLoad.pending << '/'
        << diagnostics.chunkLoad.inFlight << '/'
        << diagnostics.chunkLoad.terminalErrors
        << " mesh=" << diagnostics.mesh.pending << '/'
        << diagnostics.mesh.inFlight << '/'
        << diagnostics.meshCompletionsPending << '/'
        << diagnostics.mesh.terminalErrors
        << " eviction=" << diagnostics.eviction.pending << '/'
        << diagnostics.eviction.inFlight << '/'
        << diagnostics.eviction.terminalErrors
        << " generation_partition=" << work.generationJobsStarted << '/'
        << work.generationJobsCompleted << '/'
        << work.generationJobsCancelled << '/'
        << work.generationJobsFailed
        << " mesh_partition=" << work.meshJobsStarted << '/'
        << work.meshJobsCompleted << '/' << work.meshJobsAccepted << '/'
        << work.meshJobsRejectedStale << '/' << work.meshJobsFailed;
}

void printStartupBacklog(const OverlayStartupSnapshot& startup) {
    using State = Voxel::ChunkStreamer::DebugState;
    const auto stateCount = [&](State state) {
        return startup.states[static_cast<size_t>(state)];
    };
    const auto& diagnostics = startup.diagnostics;
    const auto& work = startup.work;
    std::cout
        << " workload=startup_backlog"
        << " startup_state="
        << Voxel::streamingLifecycleName(diagnostics.state)
        << " startup_tracked_records=" << startup.trackedRecords
        << " startup_waiting_for_data="
        << stateCount(State::WaitingForData)
        << " startup_waiting_for_neighbors="
        << stateCount(State::WaitingForNeighbors)
        << " startup_mesh_scheduler_wait="
        << stateCount(State::MeshSchedulerWait)
        << " startup_mesh_work="
        << stateCount(State::MeshSubmittedOrBuilding)
        << " startup_voxel_empty=" << stateCount(State::VoxelEmpty)
        << " startup_accepted_empty_geometry="
        << stateCount(State::AcceptedEmptyGeometry)
        << " startup_accepted_nonempty_geometry="
        << stateCount(State::AcceptedNonemptyGeometry)
        << " startup_dirty_remesh_pending="
        << stateCount(State::DirtyRemeshPending)
        << " startup_terminal_failure="
        << stateCount(State::TerminalFailure)
        << " startup_generation_started=" << work.generationJobsStarted
        << " startup_generation_completed=" << work.generationJobsCompleted
        << " startup_generation_cancelled=" << work.generationJobsCancelled
        << " startup_generation_failed=" << work.generationJobsFailed
        << " startup_generation_pending=" << diagnostics.generation.pending
        << " startup_generation_in_flight="
        << diagnostics.generation.inFlight
        << " startup_generation_completion_pending="
        << diagnostics.generationCompletionsPending
        << " startup_generation_terminal_failures="
        << diagnostics.generation.terminalErrors
        << " startup_canonical_source_work="
        << diagnostics.sourceResolutionPending
        << " startup_canonical_generation_work="
        << diagnostics.generationSchedulerPending
        << " startup_canonical_retired_work="
        << diagnostics.retiredWorkPending
        << " startup_chunk_load_pending=" << diagnostics.chunkLoad.pending
        << " startup_chunk_load_in_flight=" << diagnostics.chunkLoad.inFlight
        << " startup_chunk_load_terminal_failures="
        << diagnostics.chunkLoad.terminalErrors
        << " startup_mesh_started=" << work.meshJobsStarted
        << " startup_mesh_completed=" << work.meshJobsCompleted
        << " startup_mesh_accepted=" << work.meshJobsAccepted
        << " startup_mesh_stale=" << work.meshJobsRejectedStale
        << " startup_mesh_failed=" << work.meshJobsFailed
        << " startup_mesh_pending=" << diagnostics.mesh.pending
        << " startup_mesh_in_flight=" << diagnostics.mesh.inFlight
        << " startup_mesh_completion_pending="
        << diagnostics.meshCompletionsPending
        << " startup_mesh_terminal_failures="
        << diagnostics.mesh.terminalErrors
        << " startup_eviction_pending=" << diagnostics.eviction.pending
        << " startup_eviction_in_flight=" << diagnostics.eviction.inFlight
        << " startup_eviction_terminal_failures="
        << diagnostics.eviction.terminalErrors
        << " startup_execution_settled=true"
        << " startup_logical_backlog_classified=true";
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
    const auto preparation = prepareOverlayStreaming(view, cameraPosition);
    if (preparation != OverlayPreparationResult::Ready) {
        std::cerr << "overlay_cpu_assessment_failed "
                  << "reason=" << overlayPreparationFailure(preparation);
        if (preparation == OverlayPreparationResult::ExecutionNotSettled) {
            printUnsettledExecution(
                view.streamingDiagnostics(), view.streamingMetrics());
        }
        std::cerr << '\n';
        return false;
    }
    const auto startup = captureOverlayStartup(
        view,
        cameraChunk(cameraPosition),
        configuration.streaming.viewDistanceChunks);
    if (!overlayStartupClassified(startup)) {
        std::cerr << "overlay_cpu_assessment_failed "
                     "reason=startup_backlog_not_exactly_classified\n";
        return false;
    }

    Render::DebugState debug;
    debug.field.initialized = true;
    const bool startupEnabled = debug.overlayEnabled;
    const auto renderFrame = [&](bool enabled) {
        debug.overlayEnabled = enabled;
        const auto start = Clock::now();
        const auto presentation = Render::buildDebugFieldPresentation(
            debug, &view, cameraPosition);
        if (enabled && !presentation) {
            throw std::runtime_error(
                "enabled CPU presentation produced no draw data");
        }
        return std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
    };

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
        const bool disabledFirst = index % 2 == 0;
        const double first = renderFrame(!disabledFirst);
        const double second = renderFrame(disabledFirst);
        const double disabledValue = disabledFirst ? first : second;
        const double enabledValue = disabledFirst ? second : first;
        disabled.push_back(disabledValue);
        enabled.push_back(enabledValue);
        std::cout << "overlay_frame_sample path=isolated_cpu"
                  << " pair_index=" << index
                  << " order="
                  << (disabledFirst
                          ? "disabled_then_enabled"
                          : "enabled_then_disabled")
                  << " disabled_ms=" << disabledValue
                  << " enabled_ms=" << enabledValue << '\n';
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
              << " gl_calls=none"
              << " imgui_legend_rendering=false";
    printStartupBacklog(startup);
    printPercentiles("disabled_frame", disabledSummary);
    printPercentiles("enabled_frame", enabledSummary);
    std::cout << " p95_delta_ms="
              << enabledSummary.p95 - disabledSummary.p95 << '\n';
    return true;
}

bool runOverlayAssessment(Asset::AssetManager& assets, size_t frames) {
    OpenGLContext context;
    if (!context.ready()) {
        std::cerr << "overlay_assessment_failed reason=\""
                  << context.failure()
                  << "\" gl_renderer=unavailable gl_version=unavailable\n";
        return false;
    }
    ScopeExit assetCacheGuard([&assets]() { assets.clearCache(); });

    const auto configuration = loadShippedWorldConfiguration(assets);
    Voxel::WorldResources resources;
    ScopeExit resourcesGuard(
        [&resources]() { resources.releaseRenderResources(); });
    resources.initialize(assets);
    Voxel::World world(resources);
    Voxel::WorldView view(world, resources);
    ScopeExit viewGuard([&view]() { view.releaseRenderResources(); });
    const auto generator = std::make_shared<const Voxel::WorldGenerator>(
        resources.registry(), configuration.generation);
    world.setGenerator(generator);
    view.setGenerator(generator);
    view.initialize(assets);
    view.setStreamConfig(configuration.streaming);
    const glm::vec3 cameraPosition(16.0f, 80.0f, 16.0f);
    const auto preparation = prepareOverlayStreaming(view, cameraPosition);
    if (preparation != OverlayPreparationResult::Ready) {
        std::cerr << "overlay_assessment_failed "
                  << "reason=" << overlayPreparationFailure(preparation);
        if (preparation == OverlayPreparationResult::ExecutionNotSettled) {
            printUnsettledExecution(
                view.streamingDiagnostics(), view.streamingMetrics());
        }
        std::cerr << '\n';
        return false;
    }
    const auto startup = captureOverlayStartup(
        view,
        cameraChunk(cameraPosition),
        configuration.streaming.viewDistanceChunks);
    if (!overlayStartupClassified(startup)) {
        std::cerr << "overlay_assessment_failed "
                     "reason=startup_backlog_not_exactly_classified\n";
        return false;
    }

    Render::FrameRenderer renderer;
    ScopeExit rendererGuard([&renderer]() { renderer.release(); });
    renderer.initialize(assets);
    const bool startupEnabled = renderer.debugOverlayEnabled();
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
    std::vector<Voxel::ChunkStreamer::DebugChunkState> states;
    view.getChunkDebugStates(states, {0, 2, 0},
                             configuration.streaming.viewDistanceChunks);
    const size_t drawEvidenceRecords = static_cast<size_t>(std::count_if(
        states.begin(), states.end(),
        [](const auto& state) {
            return state.drawEvidence !=
                Voxel::ChunkStreamer::DebugDrawEvidence::NotApplicable;
        }));
    const size_t drawnRecords = static_cast<size_t>(std::count_if(
        states.begin(), states.end(),
        [](const auto& state) {
            return state.drawEvidence ==
                Voxel::ChunkStreamer::DebugDrawEvidence::Drawn;
        }));
    if (drawnRecords == 0) {
        std::cerr << "overlay_assessment_failed "
                     "reason=positive_main_pass_draw_evidence_unavailable\n";
        return false;
    }
    std::vector<double> disabled;
    std::vector<double> enabled;
    disabled.reserve(frames);
    enabled.reserve(frames);
    for (size_t index = 0; index < frames; ++index) {
        const bool disabledFirst = index % 2 == 0;
        const double first = renderFrame(!disabledFirst);
        const double second = renderFrame(disabledFirst);
        const double disabledValue = disabledFirst ? first : second;
        const double enabledValue = disabledFirst ? second : first;
        disabled.push_back(disabledValue);
        enabled.push_back(enabledValue);
        std::cout << "overlay_frame_sample path=real_gl_imgui"
                  << " pair_index=" << index
                  << " order="
                  << (disabledFirst
                          ? "disabled_then_enabled"
                          : "enabled_then_disabled")
                  << " disabled_ms=" << disabledValue
                  << " enabled_ms=" << enabledValue << '\n';
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
              << " drawn_records=" << drawnRecords
              << " startup_overlay_enabled="
              << (startupEnabled ? "true" : "false")
              << " draw_evidence_decoration=true"
              << " gl_field_rendering=true"
              << " imgui_legend_rendering=true"
              << " gl_renderer=" << std::quoted(context.renderer())
              << " gl_version=" << std::quoted(context.version());
    printStartupBacklog(startup);
    printPercentiles("disabled_frame", disabledSummary);
    printPercentiles("enabled_frame", enabledSummary);
    std::cout << " p95_delta_ms="
              << enabledSummary.p95 - disabledSummary.p95 << '\n';

    return true;
}

} // namespace

int runBenchmark(int argc, char** argv) {
    enum class Mode : uint8_t {
        All,
        Vertical,
        Overlay,
        OverlayCpu
    };
    size_t frames = 120;
    bool framesSpecified = false;
    std::optional<Mode> selectedMode;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto selectMode = [&](Mode mode) {
            if (selectedMode) {
                std::cerr << "Only one assessment mode may be selected\n";
                return false;
            }
            selectedMode = mode;
            return true;
        };
        if (argument == "--vertical-only") {
            if (!selectMode(Mode::Vertical)) {
                return 2;
            }
        } else if (argument == "--overlay-only") {
            if (!selectMode(Mode::Overlay)) {
                return 2;
            }
        } else if (argument == "--overlay-cpu-only") {
            if (!selectMode(Mode::OverlayCpu)) {
                return 2;
            }
        } else if (argument == "--frames" && index + 1 < argc) {
            if (framesSpecified) {
                std::cerr << "Frame count may be specified only once\n";
                return 2;
            }
            const std::string_view value(argv[++index]);
            unsigned long long parsed = 0;
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} ||
                end != value.data() + value.size() || parsed == 0 ||
                parsed > kMaxFramesPerMode) {
                std::cerr << "Invalid bounded frame count: " << value << '\n';
                return 2;
            }
            frames = static_cast<size_t>(parsed);
            framesSpecified = true;
        } else {
            std::cerr << "Unknown or incomplete option: " << argument << '\n';
            return 2;
        }
    }
    const Mode mode = selectedMode.value_or(Mode::All);
    if (mode == Mode::Vertical && framesSpecified) {
        std::cerr << "Frame count is unsupported for vertical-only mode\n";
        return 2;
    }
    const bool vertical = mode == Mode::All || mode == Mode::Vertical;
    const bool overlay = mode == Mode::All || mode == Mode::Overlay;
    const bool overlayCpu = mode == Mode::OverlayCpu;

    std::cout << std::fixed << std::setprecision(3)
              << "benchmark name=streaming_assessment version=2"
              << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " shipped_world_configuration=true"
              << " percentile_method=nearest_rank"
              << " p95_p99_9_sample_interpretation="
                 "observed_cohort_maximum_noisy_tail\n";
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

int main(int argc, char** argv) {
    try {
        return runBenchmark(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "streaming_assessment_failed reason=\"exception: "
                  << error.what() << "\"\n";
    } catch (...) {
        std::cerr << "streaming_assessment_failed "
                     "reason=unknown_exception\n";
    }
    return 1;
}
