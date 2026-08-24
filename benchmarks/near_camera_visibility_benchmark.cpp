#include "NearCameraVisibilityBenchmark.h"

#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"
#include "Rigel/Voxel/ChunkManager.h"
#include "Rigel/Voxel/ChunkStreamer.h"
#include "Rigel/Voxel/StreamingConfig.h"
#include "Rigel/Voxel/WorldGenerator.h"
#include "Rigel/Voxel/WorldMeshStore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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

struct Options {
    size_t samplesPerDistance = 20;
    int viewDistance = 2;
    int workerThreads = 2;
    int timeoutSeconds = 30;
    double dependencyBudgetMilliseconds = 50.0;
};

struct RunResult {
    std::optional<Benchmark::NearCameraVisibilitySample> sample;
    std::string error;
    uint64_t updateCount = 0;
    Voxel::ChunkStreamer::WorkMetrics work;
    Voxel::StreamingDiagnosticSnapshot diagnostics;
};

std::optional<long long> parseInteger(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const std::string owned(value);
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

std::optional<double> parseDouble(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const std::string owned(value);
    const double parsed = std::strtod(owned.c_str(), &end);
    if (!end || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

void printUsage() {
    std::cout
        << "Usage: Rigel_near_camera_visibility_benchmark [options]\n"
        << "  --samples N                samples for each distance (default 20)\n"
        << "  --view-distance N          cold-start view radius (default 2)\n"
        << "  --worker-threads N         production worker setting (default 2)\n"
        << "  --timeout-seconds N        per-sample safety deadline (default 30)\n"
        << "  --dependency-budget-ms N   P95 assessment threshold (default 50)\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            printUsage();
            return false;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--dependency-budget-ms") {
            const auto parsed = parseDouble(value);
            if (!parsed || *parsed <= 0.0) {
                std::cerr << "Invalid dependency budget: " << value << '\n';
                return false;
            }
            options.dependencyBudgetMilliseconds = *parsed;
            continue;
        }
        const auto parsed = parseInteger(value);
        if (!parsed || *parsed <= 0 ||
            *parsed > std::numeric_limits<int>::max()) {
            std::cerr << "Invalid positive integer: " << value << '\n';
            return false;
        }
        if (argument == "--samples") {
            options.samplesPerDistance = static_cast<size_t>(*parsed);
        } else if (argument == "--view-distance") {
            options.viewDistance = static_cast<int>(*parsed);
        } else if (argument == "--worker-threads") {
            options.workerThreads = static_cast<int>(*parsed);
        } else if (argument == "--timeout-seconds") {
            options.timeoutSeconds = static_cast<int>(*parsed);
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }
    return true;
}

std::shared_ptr<Voxel::WorldGenerator> makeGenerator(
    Voxel::BlockRegistry& registry) {
    Voxel::BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);

    Voxel::BlockType surface;
    surface.identifier = "rigel:grass";
    registry.registerBlock(surface.identifier, surface);

    Voxel::WorldGenConfig config;
    config.seed = 1;
    config.solidBlock = solid.identifier;
    config.surfaceBlock = surface.identifier;
    config.terrain.baseHeight = 0.0f;
    config.terrain.heightVariation = 0.0f;
    config.terrain.surfaceDepth = 1;
    return std::make_shared<Voxel::WorldGenerator>(
        registry, std::move(config));
}

RunResult runSample(const Options& options,
                    Voxel::ChunkCoord target,
                    int distanceSquared) {
    Voxel::ChunkManager manager;
    Voxel::BlockRegistry registry;
    Voxel::WorldMeshStore meshStore;
    auto generator = makeGenerator(registry);
    auto tracer = std::make_shared<Voxel::ChunkVisibilityTracer>(
        Voxel::ChunkVisibilityTracer::Config{target, 2});
    Voxel::ChunkStreamer streamer(
        manager, meshStore, registry, nullptr, generator);

    Voxel::StreamingConfig stream;
    stream.viewDistanceChunks = options.viewDistance;
    stream.unloadDistanceChunks = options.viewDistance;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = 0;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = options.workerThreads;
    stream.maxResidentChunks = 0;
    streamer.setConfig(stream);
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([](Voxel::ChunkLoadRequest) {
        return Voxel::ChunkLoadRequestResult::Missing;
    });
    streamer.markSpawnDiscoveryComplete();

    RunResult result;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.timeoutSeconds);
    const Voxel::ChunkCoord cameraChunk{0, 0, 0};
    while (std::chrono::steady_clock::now() < deadline) {
        streamer.update(cameraChunk.toWorldCenter());
        streamer.processCompletions();
        ++result.updateCount;
        if (streamer.diagnostics().state ==
            Voxel::StreamingLifecycleState::Quiescent) {
            break;
        }
        std::this_thread::yield();
    }

    result.work = streamer.workMetrics();
    result.diagnostics = streamer.diagnostics();
    if (result.diagnostics.state !=
        Voxel::StreamingLifecycleState::Quiescent) {
        result.error = "streaming did not reach quiescence before deadline";
        return result;
    }

    const auto measurement = tracer->measurement();
    if (measurement.stats.droppedRecords != 0 ||
        measurement.stats.droppedUnfinishedRecords != 0 ||
        measurement.stats.unmatchedEvents != 0 ||
        measurement.stats.clockFailures != 0) {
        result.error = "visibility trace accounting was not clean";
        return result;
    }
    const auto record = std::find_if(
        measurement.records.begin(),
        measurement.records.end(),
        [](const Voxel::ChunkVisibilityTraceRecord& candidate) {
            return candidate.kind ==
                Voxel::ChunkVisibilityLifecycleKind::CameraDemand;
        });
    if (record == measurement.records.end()) {
        result.error = "camera-demand visibility record was absent";
        return result;
    }
    result.sample = Benchmark::makeNearCameraVisibilitySample(
        *record, distanceSquared);
    if (!result.sample) {
        result.error = "camera-demand visibility record was incomplete";
    }
    return result;
}

double milliseconds(Voxel::ChunkVisibilityDuration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void printSummary(std::string_view kind,
                  const Benchmark::NearCameraVisibilitySummary& summary) {
    std::cout << "cohort kind=" << kind << " distance_squared=";
    if (kind == "overall") {
        std::cout << "all";
    } else {
        std::cout << summary.distanceSquared;
    }
    if (summary.dependencyCount) {
        std::cout << " dependency_count="
                  << static_cast<unsigned>(*summary.dependencyCount);
    } else {
        std::cout << " dependency_count=all";
    }
    std::cout
        << " samples=" << summary.samples
        << " accepted_endpoints=" << summary.acceptedEndpoints
        << " first_draw_endpoints=" << summary.firstDrawEndpoints
        << " desired_to_visible_p50_ms="
        << summary.desiredToVisible.p50Milliseconds
        << " desired_to_visible_p95_ms="
        << summary.desiredToVisible.p95Milliseconds
        << " dependency_wait_p50_ms="
        << summary.dependencyWait.p50Milliseconds
        << " dependency_wait_p95_ms="
        << summary.dependencyWait.p95Milliseconds
        << " eligible_to_worker_start_p50_ms="
        << summary.eligibleToWorkerStart.p50Milliseconds
        << " eligible_to_worker_start_p95_ms="
        << summary.eligibleToWorkerStart.p95Milliseconds
        << " scheduler_wait_p50_ms="
        << summary.schedulerWait.p50Milliseconds
        << " scheduler_wait_p95_ms="
        << summary.schedulerWait.p95Milliseconds
        << " pool_wait_p50_ms="
        << summary.poolWait.p50Milliseconds
        << " pool_wait_p95_ms="
        << summary.poolWait.p95Milliseconds
        << " worker_execution_p50_ms="
        << summary.workerExecution.p50Milliseconds
        << " worker_execution_p95_ms="
        << summary.workerExecution.p95Milliseconds
        << '\n';
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return argc > 1 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "benchmark name=near_camera_visibility version=1"
        << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
        << " hardware_threads=" << std::thread::hardware_concurrency()
        << " renderer=headless"
        << " visibility_endpoint=accepted"
        << " persistence=fresh_world_missing_probe"
        << " wait_signal=streaming_quiescent"
        << " fixed_startup_sleep=false\n";
    std::cout
        << "configuration samples_per_distance=" << options.samplesPerDistance
        << " view_distance=" << options.viewDistance
        << " worker_threads=" << options.workerThreads
        << " gen_queue_limit=unbounded"
        << " mesh_queue_limit_setting=unbounded"
        << " effective_mesh_submission_limit="
        << std::max(1, options.workerThreads / 2)
        << " update_budget=unbounded"
        << " apply_budget=unbounded"
        << " dependency_budget_ms="
        << options.dependencyBudgetMilliseconds << '\n';
    std::cout
        << "limitations first_draw_unavailable=true"
        << " gpu_context=false"
        << " shipped_persistence_backend=false"
        << " host_load_uncontrolled=true\n";

    std::vector<Benchmark::NearCameraVisibilitySample> samples;
    samples.reserve(options.samplesPerDistance * 2);
    const std::array targets{
        std::pair{Voxel::ChunkCoord{0, 0, 0}, 0},
        std::pair{Voxel::ChunkCoord{1, 0, 0}, 1}
    };
    for (size_t index = 0; index < options.samplesPerDistance; ++index) {
        for (const auto& [target, distanceSquared] : targets) {
            const auto result = runSample(options, target, distanceSquared);
            if (!result.sample) {
                std::cerr
                    << "sample_failed index=" << index
                    << " distance_squared=" << distanceSquared
                    << " reason=" << result.error << '\n';
                return 1;
            }
            const auto& sample = *result.sample;
            std::cout
                << "sample index=" << index
                << " distance_squared=" << distanceSquared
                << " dependency_count="
                << static_cast<unsigned>(sample.dependencyCount)
                << " endpoint="
                << (sample.endpoint == Benchmark::VisibilityEndpoint::FirstDraw
                        ? "first_draw"
                        : "accepted")
                << " desired_to_visible_ms="
                << milliseconds(sample.desiredToVisible)
                << " dependency_wait_ms="
                << milliseconds(sample.dependencyWait)
                << " eligible_to_worker_start_ms="
                << milliseconds(sample.eligibleToWorkerStart)
                << " scheduler_wait_ms="
                << milliseconds(sample.schedulerWait)
                << " pool_wait_ms=" << milliseconds(sample.poolWait)
                << " worker_execution_ms="
                << milliseconds(sample.workerExecution)
                << " lifecycle_updates=" << result.updateCount
                << " mesh_started=" << result.work.meshJobsStarted
                << " mesh_stale=" << result.work.meshJobsRejectedStale
                << " stable_updates=" << result.diagnostics.stableUpdates
                << '\n';
            samples.push_back(sample);
        }
    }

    for (const auto& [target, distanceSquared] : targets) {
        (void)target;
        std::vector<Benchmark::NearCameraVisibilitySample> distanceSamples;
        std::copy_if(
            samples.begin(),
            samples.end(),
            std::back_inserter(distanceSamples),
            [distanceSquared](const auto& sample) {
                return sample.distanceSquared == distanceSquared;
            });
        auto summary =
            Benchmark::summarizeNearCameraVisibility(distanceSamples);
        summary.distanceSquared = distanceSquared;
        printSummary("distance", summary);
    }

    const auto cohortSummaries =
        Benchmark::summarizeNearCameraVisibilityCohorts(samples);
    double maximumDependencyP95 = 0.0;
    for (const auto& summary : cohortSummaries) {
        printSummary("distance_dependency", summary);
        if (summary.dependencyCount && *summary.dependencyCount > 0) {
            maximumDependencyP95 = std::max(
                maximumDependencyP95,
                summary.dependencyWait.p95Milliseconds);
        }
    }

    auto overall = Benchmark::summarizeNearCameraVisibility(samples);
    printSummary("overall", overall);
    const bool withinBudget =
        maximumDependencyP95 <= options.dependencyBudgetMilliseconds;
    std::cout
        << "assessment six_neighbor_barrier="
        << (withinBudget ? "within_headless_budget" : "exceeds_headless_budget")
        << " maximum_near_dependency_p95_ms=" << maximumDependencyP95
        << " dependency_budget_ms="
        << options.dependencyBudgetMilliseconds
        << " provisional_neighbor_policy="
        << (withinBudget ? "not_justified" : "requires_separate_validation")
        << '\n';
    std::cout
        << "external_validation required=true"
        << " endpoint=first_draw"
        << " renderer=interactive_main_pass"
        << " persistence=shipped_backend"
        << " wait_signal=streaming_quiescent\n";
    return 0;
}
