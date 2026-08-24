#include "TestFramework.h"

#include "NearCameraVisibilityBenchmark.h"

#include <chrono>

using namespace Rigel;

namespace {

Voxel::ChunkVisibilityDuration milliseconds(int value) {
    return std::chrono::milliseconds(value);
}

Benchmark::NearCameraVisibilitySample sample(
    int distanceSquared,
    uint8_t dependencyCount,
    int value) {
    return {
        .distanceSquared = distanceSquared,
        .dependencyCount = dependencyCount,
        .endpoint = Benchmark::VisibilityEndpoint::Accepted,
        .desiredToVisible = milliseconds(value),
        .dependencyWait = milliseconds(value + 1),
        .eligibleToWorkerStart = milliseconds(value + 2),
        .schedulerWait = milliseconds(value + 3),
        .poolWait = milliseconds(value + 4),
        .workerExecution = milliseconds(value + 5)
    };
}

} // namespace

TEST_CASE(NearCameraVisibilityBenchmark_UsesNearestRankAndStableCohorts) {
    std::vector<Benchmark::NearCameraVisibilitySample> samples;
    for (int value = 1; value <= 20; ++value) {
        samples.push_back(sample(0, 6, value));
        samples.push_back(sample(1, 2, value + 100));
    }

    const auto overall =
        Benchmark::summarizeNearCameraVisibility(samples);
    CHECK_EQ(overall.samples, static_cast<size_t>(40));
    CHECK_EQ(overall.acceptedEndpoints, static_cast<size_t>(40));
    CHECK_NEAR(overall.desiredToVisible.p50Milliseconds, 20.0, 0.0001);
    CHECK_NEAR(overall.desiredToVisible.p95Milliseconds, 118.0, 0.0001);

    const auto cohorts =
        Benchmark::summarizeNearCameraVisibilityCohorts(samples);
    CHECK_EQ(cohorts.size(), static_cast<size_t>(2));
    CHECK_EQ(cohorts[0].distanceSquared, 0);
    CHECK_EQ(cohorts[0].dependencyCount, std::optional<uint8_t>{6});
    CHECK_NEAR(
        cohorts[0].desiredToVisible.p50Milliseconds, 10.0, 0.0001);
    CHECK_NEAR(
        cohorts[0].desiredToVisible.p95Milliseconds, 19.0, 0.0001);
    CHECK_EQ(cohorts[1].distanceSquared, 1);
    CHECK_EQ(cohorts[1].dependencyCount, std::optional<uint8_t>{2});
    CHECK_NEAR(
        cohorts[1].workerExecution.p50Milliseconds, 115.0, 0.0001);
    CHECK_NEAR(
        cohorts[1].workerExecution.p95Milliseconds, 124.0, 0.0001);
}

TEST_CASE(NearCameraVisibilityBenchmark_PrefersDrawAndRejectsCensoredWork) {
    Voxel::ChunkVisibilityTraceRecord record;
    record.kind = Voxel::ChunkVisibilityLifecycleKind::CameraDemand;
    record.dependencyCount = 3;
    const auto origin = Voxel::ChunkVisibilityTimePoint{};
    const auto setStage = [&](Voxel::ChunkVisibilityStage stage, int time) {
        record.stages[static_cast<size_t>(stage)] =
            origin + std::chrono::milliseconds(time);
    };
    setStage(Voxel::ChunkVisibilityStage::Desired, 0);
    setStage(Voxel::ChunkVisibilityStage::DataReady, 1);
    setStage(Voxel::ChunkVisibilityStage::NeighborReady, 4);
    setStage(Voxel::ChunkVisibilityStage::MeshEligible, 5);
    setStage(Voxel::ChunkVisibilityStage::SchedulerWait, 5);
    setStage(Voxel::ChunkVisibilityStage::PoolSubmit, 7);
    setStage(Voxel::ChunkVisibilityStage::WorkerStart, 9);
    setStage(Voxel::ChunkVisibilityStage::WorkerFinish, 11);
    setStage(Voxel::ChunkVisibilityStage::ResultAccepted, 12);
    setStage(Voxel::ChunkVisibilityStage::FirstDraw, 15);

    auto converted =
        Benchmark::makeNearCameraVisibilitySample(record, 1);
    CHECK(converted.has_value());
    CHECK_EQ(converted->endpoint, Benchmark::VisibilityEndpoint::FirstDraw);
    CHECK_EQ(converted->desiredToVisible, milliseconds(15));
    CHECK_EQ(converted->dependencyWait, milliseconds(3));
    CHECK_EQ(converted->eligibleToWorkerStart, milliseconds(4));
    CHECK_EQ(converted->workerExecution, milliseconds(2));

    record.stages[static_cast<size_t>(Voxel::ChunkVisibilityStage::FirstDraw)] =
        std::nullopt;
    converted = Benchmark::makeNearCameraVisibilitySample(record, 1);
    CHECK(converted.has_value());
    CHECK_EQ(converted->endpoint, Benchmark::VisibilityEndpoint::Accepted);
    CHECK_EQ(converted->desiredToVisible, milliseconds(12));

    record.dependencyCount = std::nullopt;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
    record.dependencyCount = 3;
    record.stages[static_cast<size_t>(Voxel::ChunkVisibilityStage::WorkerStart)] =
        std::nullopt;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
}
