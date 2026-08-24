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
        .p95Milliseconds = milliseconds(nearestRank(0.95))
    };
}

} // namespace

std::optional<NearCameraVisibilitySample> makeNearCameraVisibilitySample(
    const Voxel::ChunkVisibilityTraceRecord& record,
    int distanceSquared) {
    if (record.kind != Voxel::ChunkVisibilityLifecycleKind::CameraDemand ||
        !record.dependencyCount) {
        return std::nullopt;
    }

    const auto durations = record.durations();
    const bool firstDraw = durations.desiredToFirstDraw.has_value();
    const auto desiredToVisible = firstDraw
        ? durations.desiredToFirstDraw
        : durations.desiredToAccepted;
    if (!desiredToVisible || !durations.eligibleToWorkerStart ||
        !durations.schedulerWait || !durations.poolWait ||
        !durations.workerExecution) {
        return std::nullopt;
    }

    Duration dependencyWait{};
    if (*record.dependencyCount > 0) {
        if (!durations.dependencyWait) {
            return std::nullopt;
        }
        dependencyWait = *durations.dependencyWait;
    }

    return NearCameraVisibilitySample{
        .distanceSquared = distanceSquared,
        .dependencyCount = *record.dependencyCount,
        .endpoint = firstDraw
            ? VisibilityEndpoint::FirstDraw
            : VisibilityEndpoint::Accepted,
        .desiredToVisible = *desiredToVisible,
        .dependencyWait = dependencyWait,
        .eligibleToWorkerStart = *durations.eligibleToWorkerStart,
        .schedulerWait = *durations.schedulerWait,
        .poolWait = *durations.poolWait,
        .workerExecution = *durations.workerExecution
    };
}

NearCameraVisibilitySummary summarizeNearCameraVisibility(
    const std::vector<NearCameraVisibilitySample>& samples) {
    NearCameraVisibilitySummary summary;
    summary.samples = samples.size();
    std::vector<Duration> desiredToVisible;
    std::vector<Duration> dependencyWait;
    std::vector<Duration> eligibleToWorkerStart;
    std::vector<Duration> schedulerWait;
    std::vector<Duration> poolWait;
    std::vector<Duration> workerExecution;
    desiredToVisible.reserve(samples.size());
    dependencyWait.reserve(samples.size());
    eligibleToWorkerStart.reserve(samples.size());
    schedulerWait.reserve(samples.size());
    poolWait.reserve(samples.size());
    workerExecution.reserve(samples.size());

    for (const auto& sample : samples) {
        if (sample.endpoint == VisibilityEndpoint::FirstDraw) {
            ++summary.firstDrawEndpoints;
        } else {
            ++summary.acceptedEndpoints;
        }
        desiredToVisible.push_back(sample.desiredToVisible);
        dependencyWait.push_back(sample.dependencyWait);
        eligibleToWorkerStart.push_back(sample.eligibleToWorkerStart);
        schedulerWait.push_back(sample.schedulerWait);
        poolWait.push_back(sample.poolWait);
        workerExecution.push_back(sample.workerExecution);
    }

    summary.desiredToVisible = percentiles(std::move(desiredToVisible));
    summary.dependencyWait = percentiles(std::move(dependencyWait));
    summary.eligibleToWorkerStart =
        percentiles(std::move(eligibleToWorkerStart));
    summary.schedulerWait = percentiles(std::move(schedulerWait));
    summary.poolWait = percentiles(std::move(poolWait));
    summary.workerExecution = percentiles(std::move(workerExecution));
    return summary;
}

std::vector<NearCameraVisibilitySummary> summarizeNearCameraVisibilityCohorts(
    const std::vector<NearCameraVisibilitySample>& samples) {
    using CohortKey = std::pair<int, uint8_t>;
    std::map<CohortKey, std::vector<NearCameraVisibilitySample>> cohorts;
    for (const auto& sample : samples) {
        cohorts[{sample.distanceSquared, sample.dependencyCount}].push_back(
            sample);
    }

    std::vector<NearCameraVisibilitySummary> summaries;
    summaries.reserve(cohorts.size());
    for (auto& [key, cohort] : cohorts) {
        auto summary = summarizeNearCameraVisibility(cohort);
        summary.distanceSquared = key.first;
        summary.dependencyCount = key.second;
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

} // namespace Rigel::Benchmark
