#include "NearCameraVisibilityBenchmark.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace Rigel::Benchmark {

namespace {

using Duration = Voxel::ChunkVisibilityDuration;

DurationPercentiles percentiles(std::vector<Duration> values) {
    if (values.empty()) {
        return {};
    }
    std::sort(values.begin(), values.end());
    const auto nearestRank = [&](double quantile) {
        const size_t rank = static_cast<size_t>(
            std::ceil(quantile * static_cast<double>(values.size())));
        return values[std::max<size_t>(1, rank) - 1];
    };
    const auto milliseconds = [](Duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    return {
        .samples = values.size(),
        .p50Milliseconds = milliseconds(nearestRank(0.50)),
        .p95Milliseconds = milliseconds(nearestRank(0.95)),
        .p99Milliseconds = milliseconds(nearestRank(0.99))
    };
}

std::optional<Duration> between(
    const Voxel::ChunkVisibilityTraceRecord& record,
    Voxel::ChunkVisibilityStage from,
    Voxel::ChunkVisibilityStage to) {
    const auto start = record.stage(from);
    const auto finish = record.stage(to);
    if (!start || !finish || *finish < *start) {
        return std::nullopt;
    }
    return *finish - *start;
}

} // namespace

std::optional<NearCameraVisibilitySample> makeNearCameraVisibilitySample(
    const Voxel::ChunkVisibilityTraceRecord& record,
    int distanceSquared) {
    if (record.kind != Voxel::ChunkVisibilityLifecycleKind::CameraDemand ||
        !record.firstObservedMissingDesiredCardinalNeighborCount) {
        return std::nullopt;
    }

    const auto durations = record.durations();
    const bool firstDraw = durations.desiredToFirstDraw.has_value();
    const auto desiredToVisible = firstDraw
        ? durations.desiredToFirstDraw
        : durations.desiredToAccepted;
    const auto desiredToGenerationStart = between(
        record,
        Voxel::ChunkVisibilityStage::Desired,
        Voxel::ChunkVisibilityStage::GenerationWorkerStart);
    const auto generationQueueWait = between(
        record,
        Voxel::ChunkVisibilityStage::GenerationSchedulerPending,
        Voxel::ChunkVisibilityStage::GenerationWorkerStart);
    if (!desiredToVisible || !durations.desiredToAccepted ||
        !desiredToGenerationStart || !generationQueueWait ||
        !durations.generationQueueWait || !durations.generationPoolWait ||
        !durations.generationExecution || !durations.workerExecution) {
        return std::nullopt;
    }

    Duration dataReadyToNeighborsReady{};
    std::optional<Voxel::ChunkVisibilityTimePoint> neighborsReady =
        record.stage(Voxel::ChunkVisibilityStage::NeighborReady);
    if (*record.firstObservedMissingDesiredCardinalNeighborCount > 0) {
        if (!durations.dependencyWait) {
            return std::nullopt;
        }
        dataReadyToNeighborsReady = *durations.dependencyWait;
    } else if (!neighborsReady) {
        neighborsReady =
            record.stage(Voxel::ChunkVisibilityStage::DataReady);
    }
    const auto meshStart =
        record.stage(Voxel::ChunkVisibilityStage::WorkerStart);
    if (!neighborsReady || !meshStart || *meshStart < *neighborsReady) {
        return std::nullopt;
    }

    return NearCameraVisibilitySample{
        .distanceSquared = distanceSquared,
        .firstObservedMissingDesiredCardinalNeighborCount =
            *record.firstObservedMissingDesiredCardinalNeighborCount,
        .endpoint = firstDraw
            ? VisibilityEndpoint::FirstDraw
            : VisibilityEndpoint::Accepted,
        .desiredToVisible = *desiredToVisible,
        .desiredToGenerationStart = *desiredToGenerationStart,
        .generationQueueWait = *generationQueueWait,
        .generationSchedulerWait = *durations.generationQueueWait,
        .generationCapacityWait =
            durations.generationCapacityWait.value_or(Duration{}),
        .generationPoolWait = *durations.generationPoolWait,
        .generationExecution = *durations.generationExecution,
        .dataReadyToNeighborsReady = dataReadyToNeighborsReady,
        .neighborsReadyToMeshStart = *meshStart - *neighborsReady,
        .meshExecution = *durations.workerExecution,
        .desiredToAcceptedGeometry = *durations.desiredToAccepted,
        .desiredToFirstDraw = durations.desiredToFirstDraw
    };
}

NearCameraVisibilitySummary summarizeNearCameraVisibility(
    const std::vector<NearCameraVisibilitySample>& samples) {
    NearCameraVisibilitySummary summary;
    summary.samples = samples.size();
    std::vector<Duration> desiredToVisible;
    std::vector<Duration> desiredToGenerationStart;
    std::vector<Duration> generationQueueWait;
    std::vector<Duration> generationSchedulerWait;
    std::vector<Duration> generationCapacityWait;
    std::vector<Duration> generationPoolWait;
    std::vector<Duration> generationExecution;
    std::vector<Duration> dataReadyToNeighborsReady;
    std::vector<Duration> neighborsReadyToMeshStart;
    std::vector<Duration> meshExecution;
    std::vector<Duration> desiredToAcceptedGeometry;
    std::vector<Duration> desiredToFirstDraw;
    desiredToVisible.reserve(samples.size());
    desiredToGenerationStart.reserve(samples.size());
    generationQueueWait.reserve(samples.size());
    generationSchedulerWait.reserve(samples.size());
    generationCapacityWait.reserve(samples.size());
    generationPoolWait.reserve(samples.size());
    generationExecution.reserve(samples.size());
    dataReadyToNeighborsReady.reserve(samples.size());
    neighborsReadyToMeshStart.reserve(samples.size());
    meshExecution.reserve(samples.size());
    desiredToAcceptedGeometry.reserve(samples.size());
    desiredToFirstDraw.reserve(samples.size());

    for (const auto& sample : samples) {
        if (sample.endpoint == VisibilityEndpoint::FirstDraw) {
            ++summary.firstDrawEndpoints;
        } else {
            ++summary.acceptedEndpoints;
        }
        desiredToVisible.push_back(sample.desiredToVisible);
        desiredToGenerationStart.push_back(sample.desiredToGenerationStart);
        generationQueueWait.push_back(sample.generationQueueWait);
        generationSchedulerWait.push_back(sample.generationSchedulerWait);
        generationCapacityWait.push_back(sample.generationCapacityWait);
        generationPoolWait.push_back(sample.generationPoolWait);
        generationExecution.push_back(sample.generationExecution);
        dataReadyToNeighborsReady.push_back(
            sample.dataReadyToNeighborsReady);
        neighborsReadyToMeshStart.push_back(
            sample.neighborsReadyToMeshStart);
        meshExecution.push_back(sample.meshExecution);
        desiredToAcceptedGeometry.push_back(
            sample.desiredToAcceptedGeometry);
        if (sample.desiredToFirstDraw) {
            desiredToFirstDraw.push_back(*sample.desiredToFirstDraw);
        }
    }

    summary.desiredToVisible = percentiles(std::move(desiredToVisible));
    summary.desiredToGenerationStart =
        percentiles(std::move(desiredToGenerationStart));
    summary.generationQueueWait =
        percentiles(std::move(generationQueueWait));
    summary.generationSchedulerWait =
        percentiles(std::move(generationSchedulerWait));
    summary.generationCapacityWait =
        percentiles(std::move(generationCapacityWait));
    summary.generationPoolWait =
        percentiles(std::move(generationPoolWait));
    summary.generationExecution =
        percentiles(std::move(generationExecution));
    summary.dataReadyToNeighborsReady =
        percentiles(std::move(dataReadyToNeighborsReady));
    summary.neighborsReadyToMeshStart =
        percentiles(std::move(neighborsReadyToMeshStart));
    summary.meshExecution = percentiles(std::move(meshExecution));
    summary.desiredToAcceptedGeometry =
        percentiles(std::move(desiredToAcceptedGeometry));
    summary.desiredToFirstDraw =
        percentiles(std::move(desiredToFirstDraw));
    return summary;
}

std::vector<NearCameraVisibilitySummary> summarizeNearCameraVisibilityCohorts(
    const std::vector<NearCameraVisibilitySample>& samples) {
    using CohortKey = std::pair<int, uint8_t>;
    std::map<CohortKey, std::vector<NearCameraVisibilitySample>> cohorts;
    for (const auto& sample : samples) {
        cohorts[{
            sample.distanceSquared,
            sample.firstObservedMissingDesiredCardinalNeighborCount
        }].push_back(sample);
    }

    std::vector<NearCameraVisibilitySummary> summaries;
    summaries.reserve(cohorts.size());
    for (auto& [key, cohort] : cohorts) {
        auto summary = summarizeNearCameraVisibility(cohort);
        summary.distanceSquared = key.first;
        summary.firstObservedMissingDesiredCardinalNeighborCount = key.second;
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

} // namespace Rigel::Benchmark
