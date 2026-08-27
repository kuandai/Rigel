#include "NearCameraVisibilityBenchmark.h"

#include "Rigel/Preferences/UserPreferences.h"
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
#include <charconv>
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
constexpr size_t kMaxSamplesPerWorkload = 1000;
constexpr int kMaxMotionSteps = 100000;
constexpr int kMaxTimeoutSeconds = 3600;
constexpr long long kMaxExplicitWorkerThreads = 64;
constexpr long long kMaxExplicitQueueLimit = 32768;
constexpr double kMaxComparisonBudgetMilliseconds = 3600000.0;

struct Options {
    enum class WorkloadFilter : uint8_t {
        All,
        Stationary,
        PositiveX,
        PositiveZ,
        Diagonal
    };

    size_t samplesPerWorkload = 20;
    int viewDistance = 2;
    int workerThreads = 2;
    int meshQueueLimit = 0;
    int motionSteps = 6;
    int timeoutSeconds = 30;
    long double updateIntervalMilliseconds = kDefaultUpdateIntervalMilliseconds;
    BenchmarkClock::duration updateInterval =
        std::chrono::duration_cast<BenchmarkClock::duration>(
            std::chrono::duration<long double, std::milli>(
                kDefaultUpdateIntervalMilliseconds));
    double comparisonBudgetMilliseconds = 50.0;
    bool updateIntervalSpecified = false;
    bool schedulerLowerBoundStress = false;
    bool collectDebugDetail = false;
    WorkloadFilter workload = WorkloadFilter::All;
};

struct Workload {
    std::string_view name;
    Voxel::ChunkCoord step;
};

const std::array<Workload, 4> kWorkloads{{
    {"stationary", {0, 0, 0}},
    {"positive_x", {1, 0, 0}},
    {"positive_z", {0, 0, 1}},
    {"diagonal_xz", {1, 0, 1}}
}};

struct RunResult {
    std::optional<Benchmark::NearCameraVisibilitySample> sample;
    std::string error;
    uint64_t updateCount = 0;
    uint64_t debugDetailSnapshots = 0;
    uint64_t debugDetailRecords = 0;
    Voxel::ChunkStreamer::WorkMetrics work;
    Voxel::StreamingDiagnosticSnapshot diagnostics;
};

std::optional<long long> parseInteger(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    long long parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
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
        << "  --samples N                samples for each workload (default 20)\n"
        << "  --workload NAME            all, stationary, positive_x, positive_z,\n"
        << "                             or diagonal_xz (default all)\n"
        << "  --motion-steps N           moving-camera updates (default 6)\n"
        << "  --view-distance N          cold moving-view radius (default 2)\n"
        << "  --worker-threads N         production worker setting (default 2)\n"
        << "  --mesh-queue-limit N       configured submission cap (default unbounded)\n"
        << "  --timeout-seconds N        per-sample safety deadline (default 30)\n"
        << "  --update-interval-ms N     application-like cadence (default 16.667)\n"
        << "  --comparison-budget-ms N   operator comparison budget (default 50)\n"
        << "  --collect-debug-detail     opt in to a full debug-state snapshot after\n"
        << "                             each streaming update\n"
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
        if (argument == "--collect-debug-detail") {
            options.collectDebugDetail = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--workload") {
            if (value == "all") {
                options.workload = Options::WorkloadFilter::All;
            } else if (value == "stationary") {
                options.workload = Options::WorkloadFilter::Stationary;
            } else if (value == "positive_x") {
                options.workload = Options::WorkloadFilter::PositiveX;
            } else if (value == "positive_z") {
                options.workload = Options::WorkloadFilter::PositiveZ;
            } else if (value == "diagonal_xz") {
                options.workload = Options::WorkloadFilter::Diagonal;
            } else {
                std::cerr << "Invalid workload: " << value << '\n';
                return false;
            }
            continue;
        }
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
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0 ||
                *parsed > kMaxComparisonBudgetMilliseconds) {
                std::cerr << "Invalid positive duration: " << value << '\n';
                return false;
            }
            options.comparisonBudgetMilliseconds = *parsed;
            continue;
        }
        const auto parsed = parseInteger(value);
        if (!parsed) {
            std::cerr << "Invalid integer: " << value << '\n';
            return false;
        }
        if (argument == "--samples") {
            if (*parsed <= 0 ||
                static_cast<unsigned long long>(*parsed) >
                    kMaxSamplesPerWorkload) {
                std::cerr << "Invalid sample count: " << value << '\n';
                return false;
            }
            options.samplesPerWorkload = static_cast<size_t>(*parsed);
        } else if (argument == "--view-distance") {
            if (*parsed <= 0 ||
                *parsed > Preferences::kMaximumViewDistanceChunks) {
                std::cerr << "Unsupported view distance: " << value << '\n';
                return false;
            }
            options.viewDistance = static_cast<int>(*parsed);
        } else if (argument == "--motion-steps") {
            if (*parsed <= 0 || *parsed > kMaxMotionSteps) {
                std::cerr << "Unsafe motion step count: " << value << '\n';
                return false;
            }
            options.motionSteps = static_cast<int>(*parsed);
        } else if (argument == "--worker-threads") {
            if (*parsed <= 0 ||
                *parsed > kMaxExplicitWorkerThreads) {
                std::cerr << "Unsupported worker count: " << value << '\n';
                return false;
            }
            options.workerThreads = static_cast<int>(*parsed);
        } else if (argument == "--mesh-queue-limit") {
            if (*parsed < 0 ||
                *parsed > kMaxExplicitQueueLimit) {
                std::cerr << "Unsupported mesh queue limit: " << value << '\n';
                return false;
            }
            options.meshQueueLimit = static_cast<int>(*parsed);
        } else if (argument == "--timeout-seconds") {
            if (*parsed <= 0 || *parsed > kMaxTimeoutSeconds) {
                std::cerr << "Invalid timeout: " << value << '\n';
                return false;
            }
            options.timeoutSeconds = static_cast<int>(*parsed);
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }
    return true;
}

bool selected(const Options& options, const Workload& workload) {
    switch (options.workload) {
        case Options::WorkloadFilter::All:
            return true;
        case Options::WorkloadFilter::Stationary:
            return workload.name == "stationary";
        case Options::WorkloadFilter::PositiveX:
            return workload.name == "positive_x";
        case Options::WorkloadFilter::PositiveZ:
            return workload.name == "positive_z";
        case Options::WorkloadFilter::Diagonal:
            return workload.name == "diagonal_xz";
    }
    return false;
}

Voxel::ChunkCoord scaled(Voxel::ChunkCoord value, int scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

std::shared_ptr<Voxel::WorldGenerator> makeGenerator(
    Voxel::BlockRegistry& registry) {
    Voxel::BlockType solid;
    solid.identifier = "rigel:stone";
    registry.registerBlock(solid.identifier, solid);

    Voxel::BlockType surface;
    surface.identifier = "rigel:grass";
    registry.registerBlock(surface.identifier, surface);

    Voxel::GeneratorDefinitionData definition;
    definition.bounds = {-64, 320};
    definition.terrain = {
        .seaLevel = 0,
        .solidMaterial = solid.identifier,
        .waterMaterial = solid.identifier,
        .densityOutput = "terrain"};
    const Voxel::GeneratorDefinitionData::Noise noise{
        .octaves = 1,
        .frequency = 0.01f,
        .lacunarity = 2.0f,
        .persistence = 0.5f,
        .scale = 1.0f};
    definition.climate.global = {noise, noise, noise};
    definition.climate.local = {noise, noise, noise};
    definition.biomes.blendPower = 2.0f;
    definition.biomes.epsilon = 0.0001f;
    definition.biomes.coast = {"coast", -100.0f, 100.0f};
    Voxel::GeneratorDefinitionData::Biome biome;
    biome.id = "land";
    biome.weight = 1.0f;
    biome.surface.push_back({surface.identifier, 1});
    definition.biomes.entries.push_back(std::move(biome));
    Voxel::GeneratorDefinitionData::Biome coast;
    coast.id = "coast";
    coast.weight = 1.0f;
    coast.surface.push_back({surface.identifier, 1});
    definition.biomes.entries.push_back(std::move(coast));
    Voxel::GeneratorDefinitionData::DensityNode density;
    density.id = "flat_height";
    density.type = "y";
    density.scale = -1.0f;
    definition.densityGraph.nodes.push_back(std::move(density));
    definition.densityGraph.outputs.push_back(
        {"terrain", "flat_height"});
    return std::make_shared<Voxel::WorldGenerator>(
        registry, std::move(definition), 1u);
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

RunResult runSample(const Options& options, const Workload& workload) {
    const bool stationary = workload.step == Voxel::ChunkCoord{};
    const Voxel::ChunkCoord target = stationary
        ? Voxel::ChunkCoord{}
        : scaled(workload.step, options.motionSteps);
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
    std::vector<Voxel::ChunkStreamer::DebugChunkState> debugStates;
    while (Clock::now() < deadline) {
        if (!options.schedulerLowerBoundStress) {
            if (nextUpdate >= deadline) {
                break;
            }
            std::this_thread::sleep_until(nextUpdate);
        }
        const int motionStep = stationary
            ? 0
            : std::min<int>(
                  static_cast<int>(result.updateCount),
                  options.motionSteps);
        const Voxel::ChunkCoord cameraChunk =
            scaled(workload.step, motionStep);
        streamer.update(cameraChunk.toWorldCenter());
        streamer.processCompletions();
        if (options.collectDebugDetail) {
            streamer.getDebugStates(
                debugStates, cameraChunk, options.viewDistance);
            ++result.debugDetailSnapshots;
            result.debugDetailRecords += debugStates.size();
        }
        ++result.updateCount;
        const bool motionComplete = stationary ||
            motionStep == options.motionSteps;
        if (motionComplete && streamer.diagnostics().state ==
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
    const auto& diagnostics = result.diagnostics;
    if (!diagnostics.workEmpty() ||
        diagnostics.sourceResolutionPending != 0 ||
        diagnostics.generationSchedulerPending != 0 ||
        diagnostics.generationCompletionsPending != 0 ||
        diagnostics.meshCompletionsPending != 0 ||
        diagnostics.retiredWorkPending != 0 ||
        result.work.generationJobsStarted !=
            result.work.generationJobsCompleted +
                result.work.generationJobsCancelled ||
        result.work.generationJobsFailed >
            result.work.generationJobsCompleted ||
        result.work.meshJobsStarted != result.work.meshJobsCompleted ||
        result.work.meshJobsCompleted !=
            result.work.meshJobsAccepted +
                result.work.meshJobsRejectedStale +
                result.work.meshJobsFailed) {
        result.error = "quiescence retained streaming work ownership";
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
    result.sample = Benchmark::makeNearCameraVisibilitySample(*record, 0);
    if (!result.sample) {
        result.error = "camera-demand visibility record was incomplete";
    }
    return result;
}

double milliseconds(Voxel::ChunkVisibilityDuration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void printPercentiles(
    std::string_view name,
    const Benchmark::DurationPercentiles& percentiles) {
    std::cout << ' ' << name << "_samples=" << percentiles.samples;
    if (percentiles.samples == 0) {
        std::cout
            << ' ' << name << "_p50_ms=unavailable"
            << ' ' << name << "_p95_ms=unavailable"
            << ' ' << name << "_p99_ms=unavailable";
        return;
    }
    std::cout
        << ' ' << name << "_p50_ms=" << percentiles.p50Milliseconds
        << ' ' << name << "_p95_ms=" << percentiles.p95Milliseconds
        << ' ' << name << "_p99_ms=" << percentiles.p99Milliseconds;
}

void printSummary(
    std::string_view workload,
    const Benchmark::NearCameraVisibilitySummary& summary) {
    std::cout << "cohort workload=" << workload;
    if (summary.firstObservedMissingDesiredCardinalNeighborCount) {
        std::cout << " first_observed_missing_desired_cardinal_neighbors="
                  << static_cast<unsigned>(
                         *summary
                              .firstObservedMissingDesiredCardinalNeighborCount);
    } else {
        std::cout
            << " first_observed_missing_desired_cardinal_neighbors=all";
    }
    std::cout << " data_ready_to_neighbors_ready_boundary=";
    if (!summary.dependencyReadyBoundary) {
        std::cout << "mixed";
    } else if (*summary.dependencyReadyBoundary ==
               Benchmark::DependencyReadyBoundary::InferredDataReady) {
        std::cout << "inferred_data_ready";
    } else {
        std::cout << "observed_final_neighbor";
    }
    std::cout
        << " samples=" << summary.samples
        << " accepted_endpoints=" << summary.acceptedEndpoints
        << " first_draw_endpoints=" << summary.firstDrawEndpoints;
    printPercentiles(
        "desired_to_generation_start",
        summary.desiredToGenerationStart);
    printPercentiles("generation_queue_wait", summary.generationQueueWait);
    printPercentiles(
        "generation_scheduler_wait",
        summary.generationSchedulerWait);
    printPercentiles(
        "generation_capacity_wait",
        summary.generationCapacityWait);
    printPercentiles(
        "generation_pool_wait",
        summary.generationPoolWait);
    printPercentiles(
        "generation_execution",
        summary.generationExecution);
    printPercentiles(
        "data_ready_to_neighbors_ready",
        summary.dataReadyToNeighborsReady);
    printPercentiles(
        "neighbors_ready_to_mesh_start",
        summary.neighborsReadyToMeshStart);
    printPercentiles("mesh_execution", summary.meshExecution);
    printPercentiles(
        "desired_to_accepted_geometry",
        summary.desiredToAcceptedGeometry);
    printPercentiles("desired_to_first_draw", summary.desiredToFirstDraw);
    std::cout << '\n';
}

} // namespace

int runBenchmark(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return argc > 1 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }

    const auto schedulerMetadata = runtimeSchedulerMetadata(options);

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "benchmark name=near_camera_visibility version=5"
        << " build_type=" << RIGEL_BENCHMARK_BUILD_TYPE
        << " hardware_threads=" << std::thread::hardware_concurrency()
        << " fixture=controlled_cold_moving_generation"
        << " renderer=headless"
        << " visibility_endpoint=accepted"
        << " persistence=controlled_missing_probe"
        << " scheduling_context=not_shipped_or_interactive"
        << " wait_signal=streaming_quiescent"
        << " percentile_method=nearest_rank"
        << " p99_20_sample_interpretation=observed_cohort_maximum_noisy_tail"
        << " fixed_startup_sleep=false\n";
    std::cout
        << "configuration samples_per_workload="
        << options.samplesPerWorkload
        << " workload_filter=";
    switch (options.workload) {
        case Options::WorkloadFilter::All:
            std::cout << "all";
            break;
        case Options::WorkloadFilter::Stationary:
            std::cout << "stationary";
            break;
        case Options::WorkloadFilter::PositiveX:
            std::cout << "positive_x";
            break;
        case Options::WorkloadFilter::PositiveZ:
            std::cout << "positive_z";
            break;
        case Options::WorkloadFilter::Diagonal:
            std::cout << "diagonal_xz";
            break;
    }
    const size_t totalWorkers = static_cast<size_t>(options.workerThreads);
    const size_t meshWorkers = totalWorkers / 2;
    const size_t generationWorkers = totalWorkers - meshWorkers;
    std::cout
        << " motion_steps=" << options.motionSteps
        << " view_distance=" << options.viewDistance
        << " worker_threads=" << options.workerThreads
        << " generation_worker_threads=" << generationWorkers
        << " mesh_worker_threads=" << meshWorkers
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
        << " debug_detail_collection="
        << (options.collectDebugDetail
                ? "per_update_opt_in"
                : "disabled")
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
        << " overlay_rendering=false"
        << " shipped_persistence_backend=false"
        << " host_load_uncontrolled=true\n";

    double maximumDependencyP95 = 0.0;
    std::vector<Benchmark::NearCameraVisibilitySample> allSamples;
    allSamples.reserve(options.samplesPerWorkload * kWorkloads.size());
    for (const Workload& workload : kWorkloads) {
        if (!selected(options, workload)) {
            continue;
        }
        std::vector<Benchmark::NearCameraVisibilitySample> workloadSamples;
        workloadSamples.reserve(options.samplesPerWorkload);
        for (size_t index = 0;
             index < options.samplesPerWorkload;
             ++index) {
            const auto result = runSample(options, workload);
            if (!result.sample) {
                std::cerr
                    << "sample_failed index=" << index
                    << " workload=" << workload.name
                    << " reason=" << result.error << '\n';
                return 1;
            }
            const auto& sample = *result.sample;
            std::cout
                << "sample index=" << index
                << " workload=" << workload.name
                << " first_observed_missing_desired_cardinal_neighbors="
                << static_cast<unsigned>(
                       sample.firstObservedMissingDesiredCardinalNeighborCount)
                << " data_ready_to_neighbors_ready_boundary="
                << (sample.dependencyReadyBoundary ==
                            Benchmark::DependencyReadyBoundary::InferredDataReady
                        ? "inferred_data_ready"
                        : "observed_final_neighbor")
                << " endpoint="
                << (sample.endpoint == Benchmark::VisibilityEndpoint::FirstDraw
                        ? "first_draw"
                        : "accepted")
                << " desired_to_visible_ms="
                << milliseconds(sample.desiredToVisible)
                << " desired_to_generation_start_ms="
                << milliseconds(sample.desiredToGenerationStart)
                << " generation_queue_wait_ms="
                << milliseconds(sample.generationQueueWait)
                << " generation_scheduler_wait_ms="
                << milliseconds(sample.generationSchedulerWait)
                << " generation_capacity_wait_ms="
                << milliseconds(sample.generationCapacityWait)
                << " generation_pool_wait_ms="
                << milliseconds(sample.generationPoolWait)
                << " generation_execution_ms="
                << milliseconds(sample.generationExecution)
                << " data_ready_to_neighbors_ready_ms="
                << milliseconds(sample.dataReadyToNeighborsReady)
                << " neighbors_ready_to_mesh_start_ms="
                << milliseconds(sample.neighborsReadyToMeshStart)
                << " mesh_execution_ms="
                << milliseconds(sample.meshExecution)
                << " desired_to_accepted_geometry_ms="
                << milliseconds(sample.desiredToAcceptedGeometry)
                << " desired_to_first_draw_ms=";
            if (sample.desiredToFirstDraw) {
                std::cout << milliseconds(*sample.desiredToFirstDraw);
            } else {
                std::cout << "unavailable";
            }
            std::cout
                << " lifecycle_updates=" << result.updateCount
                << " debug_detail_snapshots="
                << result.debugDetailSnapshots
                << " debug_detail_records=" << result.debugDetailRecords
                << " generation_started="
                << result.work.generationJobsStarted
                << " generation_completed="
                << result.work.generationJobsCompleted
                << " generation_cancelled="
                << result.work.generationJobsCancelled
                << " generation_failed="
                << result.work.generationJobsFailed
                << " generation_pending="
                << result.diagnostics.generation.pending
                << " generation_in_flight="
                << result.diagnostics.generation.inFlight
                << " generation_completion_pending="
                << result.diagnostics.generationCompletionsPending
                << " generation_terminal_failures="
                << result.diagnostics.generation.terminalErrors
                << " canonical_source_work="
                << result.diagnostics.sourceResolutionPending
                << " canonical_generation_work="
                << result.diagnostics.generationSchedulerPending
                << " canonical_retired_work="
                << result.diagnostics.retiredWorkPending
                << " mesh_started=" << result.work.meshJobsStarted
                << " mesh_completed=" << result.work.meshJobsCompleted
                << " mesh_accepted=" << result.work.meshJobsAccepted
                << " mesh_stale=" << result.work.meshJobsRejectedStale
                << " mesh_failed=" << result.work.meshJobsFailed
                << " mesh_pending=" << result.diagnostics.mesh.pending
                << " mesh_in_flight=" << result.diagnostics.mesh.inFlight
                << " mesh_completion_pending="
                << result.diagnostics.meshCompletionsPending
                << " mesh_terminal_failures="
                << result.diagnostics.mesh.terminalErrors
                << " chunk_load_pending="
                << result.diagnostics.chunkLoad.pending
                << " chunk_load_in_flight="
                << result.diagnostics.chunkLoad.inFlight
                << " chunk_load_terminal_failures="
                << result.diagnostics.chunkLoad.terminalErrors
                << " eviction_pending="
                << result.diagnostics.eviction.pending
                << " eviction_in_flight="
                << result.diagnostics.eviction.inFlight
                << " eviction_terminal_failures="
                << result.diagnostics.eviction.terminalErrors
                << " stable_updates=" << result.diagnostics.stableUpdates
                << " completion_state="
                << (result.diagnostics.state ==
                            Voxel::StreamingLifecycleState::Quiescent
                        ? "quiescent"
                        : "non_quiescent")
                << '\n';
            workloadSamples.push_back(sample);
            allSamples.push_back(sample);
        }
        printSummary(
            workload.name,
            Benchmark::summarizeNearCameraVisibility(workloadSamples));
        for (const auto& summary :
             Benchmark::summarizeNearCameraVisibilityCohorts(
                 workloadSamples)) {
            printSummary(workload.name, summary);
            maximumDependencyP95 = std::max(
                maximumDependencyP95,
                summary.dataReadyToNeighborsReady.p95Milliseconds);
        }
    }

    auto overall = Benchmark::summarizeNearCameraVisibility(allSamples);
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

int main(int argc, char** argv) {
    try {
        return runBenchmark(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "near_camera_visibility_benchmark_failed "
                  << "reason=\"exception: " << error.what() << "\"\n";
    } catch (...) {
        std::cerr << "near_camera_visibility_benchmark_failed "
                     "reason=unknown_exception\n";
    }
    return 1;
}
