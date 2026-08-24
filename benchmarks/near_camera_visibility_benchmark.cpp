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
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
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
using BenchmarkClock = std::chrono::steady_clock;

constexpr long double kDefaultUpdateIntervalMilliseconds = 1000.0L / 60.0L;

struct Options {
    size_t samplesPerDistance = 20;
    int viewDistance = 2;
    int workerThreads = 2;
    int meshQueueLimit = 0;
    int timeoutSeconds = 30;
    long double updateIntervalMilliseconds = kDefaultUpdateIntervalMilliseconds;
    BenchmarkClock::duration updateInterval =
        std::chrono::duration_cast<BenchmarkClock::duration>(
            std::chrono::duration<long double, std::milli>(
                kDefaultUpdateIntervalMilliseconds));
    double comparisonBudgetMilliseconds = 50.0;
    bool updateIntervalSpecified = false;
    bool schedulerLowerBoundStress = false;
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

std::optional<long double> parseLongDouble(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const std::string owned(value);
    const long double parsed = std::strtold(owned.c_str(), &end);
    if (!end || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

std::optional<BenchmarkClock::duration> toUpdateInterval(
    long double milliseconds) {
    using Milliseconds = std::chrono::duration<long double, std::milli>;
    const Milliseconds requested{milliseconds};
    const Milliseconds minimum{BenchmarkClock::duration{1}};
    auto maximumDuration = BenchmarkClock::duration::max();
    const auto elapsed = BenchmarkClock::now().time_since_epoch();
    if (elapsed > BenchmarkClock::duration::zero()) {
        maximumDuration -= elapsed;
    }
    const Milliseconds maximum{maximumDuration};
    if (!std::isfinite(milliseconds) || requested < minimum ||
        requested >= maximum) {
        return std::nullopt;
    }

    const auto interval =
        std::chrono::duration_cast<BenchmarkClock::duration>(
            requested);
    if (interval <= BenchmarkClock::duration::zero()) {
        return std::nullopt;
    }
    return interval;
}

void printUsage() {
    std::cout
        << "Usage: Rigel_near_camera_visibility_benchmark [options]\n"
        << "  --samples N                samples for each distance (default 20)\n"
        << "  --view-distance N          cold-start view radius (default 2)\n"
        << "  --worker-threads N         production worker setting (default 2)\n"
        << "  --mesh-queue-limit N       configured submission cap (default unbounded)\n"
        << "  --timeout-seconds N        per-sample safety deadline (default 30)\n"
        << "  --update-interval-ms N     application-like cadence (default 16.667)\n"
        << "  --comparison-budget-ms N   operator comparison budget (default 50)\n"
        << "  --scheduler-lower-bound-stress\n"
        << "                             run unpaced scheduler stress; timings are\n"
        << "                             not representative time-to-visible evidence\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            printUsage();
            return false;
        }
        if (argument == "--scheduler-lower-bound-stress") {
            if (options.updateIntervalSpecified) {
                std::cerr
                    << "Scheduler stress cannot be combined with an explicit "
                       "update interval\n";
                return false;
            }
            options.schedulerLowerBoundStress = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--update-interval-ms") {
            const auto parsed = parseLongDouble(value);
            const auto interval = parsed
                ? toUpdateInterval(*parsed)
                : std::nullopt;
            if (!interval) {
                std::cerr
                    << "Invalid application-like update interval: "
                    << value
                    << " (must be at least one steady-clock tick and below "
                       "the maximum schedulable steady-clock interval)\n";
                return false;
            }
            if (options.schedulerLowerBoundStress) {
                std::cerr
                    << "An explicit update interval cannot be combined "
                       "with scheduler stress\n";
                return false;
            }
            options.updateIntervalMilliseconds = *parsed;
            options.updateInterval = *interval;
            options.updateIntervalSpecified = true;
            continue;
        }
        if (argument == "--comparison-budget-ms") {
            const auto parsed = parseDouble(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0) {
                std::cerr << "Invalid positive duration: " << value << '\n';
                return false;
            }
            options.comparisonBudgetMilliseconds = *parsed;
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
        } else if (argument == "--mesh-queue-limit") {
            options.meshQueueLimit = static_cast<int>(*parsed);
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

Voxel::StreamingConfig makeStreamingConfig(const Options& options) {
    Voxel::StreamingConfig stream;
    stream.viewDistanceChunks = options.viewDistance;
    stream.unloadDistanceChunks = options.viewDistance;
    stream.genQueueLimit = 0;
    stream.meshQueueLimit = options.meshQueueLimit;
    stream.updateBudgetPerFrame = 0;
    stream.applyBudgetPerFrame = 0;
    stream.workerThreads = options.workerThreads;
    stream.maxResidentChunks = 0;
    return stream;
}

Benchmark::MeshSubmissionLimitMetadata runtimeSchedulerMetadata(
    const Options& options) {
    Voxel::ChunkManager manager;
    Voxel::BlockRegistry registry;
    Voxel::WorldMeshStore meshStore;
    Voxel::ChunkStreamer streamer(manager, meshStore, registry, nullptr, {});
    streamer.setConfig(makeStreamingConfig(options));
    return Benchmark::meshSubmissionLimitMetadata(
        streamer.diagnostics(),
        static_cast<size_t>(options.meshQueueLimit));
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

    streamer.setConfig(makeStreamingConfig(options));
    streamer.setVisibilityTracer(tracer);
    streamer.setChunkLoader([](Voxel::ChunkLoadRequest) {
        return Voxel::ChunkLoadRequestResult::Missing;
    });
    streamer.markSpawnDiscoveryComplete();

    RunResult result;
    using Clock = BenchmarkClock;
    const auto deadline = Clock::now() +
        std::chrono::seconds(options.timeoutSeconds);
    auto nextUpdate = Clock::now();
    const Voxel::ChunkCoord cameraChunk{0, 0, 0};
    while (Clock::now() < deadline) {
        if (!options.schedulerLowerBoundStress) {
            if (nextUpdate >= deadline) {
                break;
            }
            std::this_thread::sleep_until(nextUpdate);
        }
        streamer.update(cameraChunk.toWorldCenter());
        streamer.processCompletions();
        ++result.updateCount;
        if (streamer.diagnostics().state ==
            Voxel::StreamingLifecycleState::Quiescent) {
            break;
        }
        if (options.schedulerLowerBoundStress) {
            std::this_thread::yield();
        } else {
            const auto timeUntilDeadline = deadline - nextUpdate;
            if (options.updateInterval >= timeUntilDeadline) {
                nextUpdate = deadline;
            } else {
                nextUpdate += options.updateInterval;
            }
            const auto now = Clock::now();
            if (nextUpdate < now) {
                nextUpdate = now;
            }
        }
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
    if (summary.firstObservedMissingDesiredCardinalNeighborCount) {
        std::cout << " first_observed_missing_desired_cardinal_neighbors="
                  << static_cast<unsigned>(
                         *summary
                              .firstObservedMissingDesiredCardinalNeighborCount);
    } else {
        std::cout
            << " first_observed_missing_desired_cardinal_neighbors=all";
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

    const auto schedulerMetadata = runtimeSchedulerMetadata(options);

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "benchmark name=near_camera_visibility version=3"
        << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
        << " hardware_threads=" << std::thread::hardware_concurrency()
        << " fixture=controlled_cold_generation"
        << " renderer=headless"
        << " visibility_endpoint=accepted"
        << " persistence=controlled_missing_probe"
        << " scheduling_context=not_shipped_or_interactive"
        << " wait_signal=streaming_quiescent"
        << " fixed_startup_sleep=false\n";
    std::cout
        << "configuration samples_per_distance=" << options.samplesPerDistance
        << " view_distance=" << options.viewDistance
        << " worker_threads=" << options.workerThreads
        << " gen_queue_limit=unbounded"
        << " mesh_queue_limit_setting=";
    if (options.meshQueueLimit > 0) {
        std::cout << options.meshQueueLimit;
    } else {
        std::cout << "unbounded";
    }
    std::cout
        << " mesh_submission_limit_source="
        << (schedulerMetadata.source ==
                    Benchmark::MeshSubmissionLimitSource::RuntimeDiagnostics
                ? "runtime_diagnostics"
                : "runtime_configuration")
        << " mesh_submission_behavior=";
    switch (schedulerMetadata.behavior) {
        case Benchmark::MeshSubmissionBehavior::ConfiguredUnbounded:
            std::cout << "configured_unbounded";
            break;
        case Benchmark::MeshSubmissionBehavior::ConfiguredBounded:
            std::cout << "configured_bounded";
            break;
        case Benchmark::MeshSubmissionBehavior::EffectiveBounded:
            std::cout << "effective_bounded";
            break;
    }
    std::cout << " effective_mesh_submission_limit=";
    if (schedulerMetadata.effectiveLimit) {
        std::cout << *schedulerMetadata.effectiveLimit;
    } else {
        std::cout << "unavailable";
    }
    std::cout
        << " update_budget=unbounded"
        << " apply_budget=unbounded"
        << " cadence_mode="
        << (options.schedulerLowerBoundStress
                ? "scheduler_lower_bound_stress"
                : "application_like")
        << " update_interval_ms=";
    if (options.schedulerLowerBoundStress) {
        std::cout << "unpaced";
    } else {
        std::cout << options.updateIntervalMilliseconds;
    }
    std::cout
        << " evidence_scope="
        << (options.schedulerLowerBoundStress
                ? "nonrepresentative_scheduler_lower_bound"
                : "controlled_fixture_application_like_cadence")
        << " shipped_time_to_visible_evidence=false"
        << " interactive_time_to_visible_evidence=false"
        << " comparison_budget_ms="
        << options.comparisonBudgetMilliseconds
        << " comparison_budget_role=operator_supplied\n";
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
                << " first_observed_missing_desired_cardinal_neighbors="
                << static_cast<unsigned>(
                       sample.firstObservedMissingDesiredCardinalNeighborCount)
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
                << " completion_state="
                << (result.diagnostics.state ==
                            Voxel::StreamingLifecycleState::Quiescent
                        ? "quiescent"
                        : "non_quiescent")
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
        if (summary.firstObservedMissingDesiredCardinalNeighborCount &&
            *summary.firstObservedMissingDesiredCardinalNeighborCount > 0) {
            maximumDependencyP95 = std::max(
                maximumDependencyP95,
                summary.dependencyWait.p95Milliseconds);
        }
    }

    auto overall = Benchmark::summarizeNearCameraVisibility(samples);
    printSummary("overall", overall);
    const bool withinBudget =
        maximumDependencyP95 <= options.comparisonBudgetMilliseconds;
    std::cout
        << "assessment comparison_budget_status="
        << (withinBudget ? "within" : "exceeds")
        << " maximum_dependency_wait_p95_ms=" << maximumDependencyP95
        << " comparison_budget_ms="
        << options.comparisonBudgetMilliseconds
        << " comparison_budget_role=operator_supplied_comparison_only"
        << " assessment_scope=numeric_result_only"
        << '\n';
    std::cout
        << "external_validation required=true"
        << " endpoint=first_draw"
        << " renderer=interactive_main_pass"
        << " persistence=shipped_backend"
        << " wait_signal=streaming_quiescent\n";
    return 0;
}
