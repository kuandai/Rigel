#pragma once

#include "Rigel/Voxel/ChunkVisibilityTrace.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Rigel::Benchmark {

enum class VisibilityEndpoint : uint8_t {
    Accepted,
    FirstDraw
};

struct NearCameraVisibilitySample {
    int distanceSquared = 0;
    uint8_t firstObservedMissingDesiredCardinalNeighborCount = 0;
    VisibilityEndpoint endpoint = VisibilityEndpoint::Accepted;
    Voxel::ChunkVisibilityDuration desiredToVisible{};
    Voxel::ChunkVisibilityDuration dependencyWait{};
    Voxel::ChunkVisibilityDuration eligibleToWorkerStart{};
    Voxel::ChunkVisibilityDuration schedulerWait{};
    Voxel::ChunkVisibilityDuration poolWait{};
    Voxel::ChunkVisibilityDuration workerExecution{};
};

struct DurationPercentiles {
    size_t samples = 0;
    double p50Milliseconds = 0.0;
    double p95Milliseconds = 0.0;
};

struct NearCameraVisibilitySummary {
    int distanceSquared = 0;
    std::optional<uint8_t> firstObservedMissingDesiredCardinalNeighborCount;
    size_t samples = 0;
    size_t acceptedEndpoints = 0;
    size_t firstDrawEndpoints = 0;
    DurationPercentiles desiredToVisible;
    DurationPercentiles dependencyWait;
    DurationPercentiles eligibleToWorkerStart;
    DurationPercentiles schedulerWait;
    DurationPercentiles poolWait;
    DurationPercentiles workerExecution;
};

std::optional<NearCameraVisibilitySample> makeNearCameraVisibilitySample(
    const Voxel::ChunkVisibilityTraceRecord& record,
    int distanceSquared);

NearCameraVisibilitySummary summarizeNearCameraVisibility(
    const std::vector<NearCameraVisibilitySample>& samples);

std::vector<NearCameraVisibilitySummary> summarizeNearCameraVisibilityCohorts(
    const std::vector<NearCameraVisibilitySample>& samples);

} // namespace Rigel::Benchmark
