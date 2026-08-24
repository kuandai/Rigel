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
    uint8_t firstObservedMissingDesiredCardinalNeighborCount,
    int value) {
    return {
        .distanceSquared = distanceSquared,
        .firstObservedMissingDesiredCardinalNeighborCount =
            firstObservedMissingDesiredCardinalNeighborCount,
        .dependencyReadyBoundary =
            firstObservedMissingDesiredCardinalNeighborCount == 0
                ? Benchmark::DependencyReadyBoundary::InferredDataReady
                : Benchmark::DependencyReadyBoundary::ObservedFinalNeighbor,
        .endpoint = Benchmark::VisibilityEndpoint::Accepted,
        .desiredToVisible = milliseconds(value),
        .desiredToGenerationStart = milliseconds(value + 1),
        .generationQueueWait = milliseconds(value + 2),
        .generationSchedulerWait = milliseconds(value + 3),
        .generationCapacityWait = milliseconds(value + 4),
        .generationPoolWait = milliseconds(value + 5),
        .generationExecution = milliseconds(value + 6),
        .dataReadyToNeighborsReady = milliseconds(value + 7),
        .neighborsReadyToMeshStart = milliseconds(value + 8),
        .meshExecution = milliseconds(value + 9),
        .desiredToAcceptedGeometry = milliseconds(value + 10)
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
    CHECK_NEAR(overall.desiredToVisible.p99Milliseconds, 120.0, 0.0001);

    const auto cohorts =
        Benchmark::summarizeNearCameraVisibilityCohorts(samples);
    CHECK_EQ(cohorts.size(), static_cast<size_t>(2));
    CHECK_EQ(cohorts[0].distanceSquared, 0);
    CHECK_EQ(
        cohorts[0].firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{6});
    CHECK_NEAR(
        cohorts[0].desiredToVisible.p50Milliseconds, 10.0, 0.0001);
    CHECK_NEAR(
        cohorts[0].desiredToVisible.p95Milliseconds, 19.0, 0.0001);
    CHECK_EQ(cohorts[1].distanceSquared, 1);
    CHECK_EQ(
        cohorts[1].firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{2});
    CHECK_NEAR(
        cohorts[1].meshExecution.p50Milliseconds, 119.0, 0.0001);
    CHECK_NEAR(
        cohorts[1].meshExecution.p95Milliseconds, 128.0, 0.0001);
    CHECK_NEAR(
        cohorts[1].meshExecution.p99Milliseconds, 129.0, 0.0001);
}

TEST_CASE(NearCameraVisibilityBenchmark_DistinguishesSchedulerLimitMetadata) {
    struct BaselineDiagnostics {};
    struct RepairedDiagnostics {
        size_t meshSubmissionLimit = 0;
    };

    const auto baseline = Benchmark::meshSubmissionLimitMetadata(
        BaselineDiagnostics{}, 0);
    CHECK_EQ(
        baseline.source,
        Benchmark::MeshSubmissionLimitSource::RuntimeConfiguration);
    CHECK_EQ(
        baseline.behavior,
        Benchmark::MeshSubmissionBehavior::ConfiguredUnbounded);
    CHECK(!baseline.effectiveLimit.has_value());

    const auto configuredBaseline =
        Benchmark::meshSubmissionLimitMetadata(BaselineDiagnostics{}, 5);
    CHECK_EQ(
        configuredBaseline.behavior,
        Benchmark::MeshSubmissionBehavior::ConfiguredBounded);
    CHECK(!configuredBaseline.effectiveLimit.has_value());

    const auto repaired = Benchmark::meshSubmissionLimitMetadata(
        RepairedDiagnostics{3}, 0);
    CHECK_EQ(
        repaired.source,
        Benchmark::MeshSubmissionLimitSource::RuntimeDiagnostics);
    CHECK_EQ(
        repaired.behavior,
        Benchmark::MeshSubmissionBehavior::EffectiveBounded);
    CHECK_EQ(repaired.effectiveLimit, std::optional<size_t>{3});
}

TEST_CASE(NearCameraVisibilityBenchmark_PrefersDrawAndRejectsCensoredWork) {
    Voxel::ChunkVisibilityTraceRecord record;
    record.kind = Voxel::ChunkVisibilityLifecycleKind::CameraDemand;
    record.origin = Voxel::ChunkVisibilityOrigin::Generated;
    record.outcome =
        Voxel::ChunkVisibilityOutcome::AcceptedNonemptyGeometry;
    record.drawOutcome = Voxel::ChunkVisibilityDrawOutcome::Drawn;
    record.firstObservedMissingDesiredCardinalNeighborCount = 3;
    const auto origin = Voxel::ChunkVisibilityTimePoint{};
    const auto setStage = [&](Voxel::ChunkVisibilityStage stage, int time) {
        record.stages[static_cast<size_t>(stage)] =
            origin + std::chrono::milliseconds(time);
    };
    setStage(Voxel::ChunkVisibilityStage::Desired, 0);
    setStage(Voxel::ChunkVisibilityStage::GenerationSchedulerPending, 1);
    setStage(Voxel::ChunkVisibilityStage::GenerationExecutorSubmit, 3);
    setStage(Voxel::ChunkVisibilityStage::GenerationWorkerStart, 5);
    setStage(Voxel::ChunkVisibilityStage::GenerationWorkerFinish, 7);
    setStage(Voxel::ChunkVisibilityStage::GenerationReady, 7);
    setStage(Voxel::ChunkVisibilityStage::DataReady, 8);
    setStage(Voxel::ChunkVisibilityStage::NeighborReady, 11);
    setStage(Voxel::ChunkVisibilityStage::MeshEligible, 12);
    setStage(Voxel::ChunkVisibilityStage::SchedulerWait, 12);
    setStage(Voxel::ChunkVisibilityStage::PoolSubmit, 14);
    setStage(Voxel::ChunkVisibilityStage::WorkerStart, 16);
    setStage(Voxel::ChunkVisibilityStage::WorkerFinish, 18);
    setStage(Voxel::ChunkVisibilityStage::ResultAccepted, 19);
    setStage(Voxel::ChunkVisibilityStage::FirstDraw, 22);

    auto converted =
        Benchmark::makeNearCameraVisibilitySample(record, 1);
    CHECK(converted.has_value());
    CHECK_EQ(converted->endpoint, Benchmark::VisibilityEndpoint::FirstDraw);
    CHECK_EQ(converted->desiredToVisible, milliseconds(22));
    CHECK_EQ(converted->desiredToGenerationStart, milliseconds(5));
    CHECK_EQ(converted->generationQueueWait, milliseconds(4));
    CHECK_EQ(converted->generationSchedulerWait, milliseconds(2));
    CHECK_EQ(converted->generationCapacityWait, milliseconds(0));
    CHECK_EQ(converted->generationPoolWait, milliseconds(2));
    CHECK_EQ(converted->generationExecution, milliseconds(2));
    CHECK_EQ(converted->dataReadyToNeighborsReady, milliseconds(3));
    CHECK_EQ(converted->neighborsReadyToMeshStart, milliseconds(5));
    CHECK_EQ(converted->meshExecution, milliseconds(2));
    CHECK_EQ(converted->desiredToAcceptedGeometry, milliseconds(19));
    CHECK_EQ(converted->desiredToFirstDraw, milliseconds(22));

    record.stages[static_cast<size_t>(Voxel::ChunkVisibilityStage::FirstDraw)] =
        std::nullopt;
    converted = Benchmark::makeNearCameraVisibilitySample(record, 1);
    CHECK(converted.has_value());
    CHECK_EQ(converted->endpoint, Benchmark::VisibilityEndpoint::Accepted);
    CHECK_EQ(converted->desiredToVisible, milliseconds(19));
    CHECK(!converted->desiredToFirstDraw.has_value());

    record.firstObservedMissingDesiredCardinalNeighborCount = std::nullopt;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
    record.firstObservedMissingDesiredCardinalNeighborCount = 3;
    record.stages[static_cast<size_t>(Voxel::ChunkVisibilityStage::WorkerStart)] =
        std::nullopt;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
}

TEST_CASE(NearCameraVisibilityBenchmark_RequiresGeneratedNonemptyAcceptance) {
    Voxel::ChunkVisibilityTraceRecord record;
    record.kind = Voxel::ChunkVisibilityLifecycleKind::CameraDemand;
    record.origin = Voxel::ChunkVisibilityOrigin::Generated;
    record.outcome =
        Voxel::ChunkVisibilityOutcome::AcceptedNonemptyGeometry;
    record.firstObservedMissingDesiredCardinalNeighborCount = 0;
    const auto origin = Voxel::ChunkVisibilityClock::now();
    const auto setStage = [&](Voxel::ChunkVisibilityStage stage, int time) {
        record.stages[static_cast<size_t>(stage)] =
            origin + std::chrono::milliseconds(time);
    };
    setStage(Voxel::ChunkVisibilityStage::Desired, 0);
    setStage(Voxel::ChunkVisibilityStage::GenerationSchedulerPending, 1);
    setStage(Voxel::ChunkVisibilityStage::GenerationCapacityWait, 3);
    record.observedStages[static_cast<size_t>(
        Voxel::ChunkVisibilityStage::GenerationCapacityWait)] = true;
    setStage(Voxel::ChunkVisibilityStage::GenerationExecutorSubmit, 5);
    setStage(Voxel::ChunkVisibilityStage::GenerationWorkerStart, 7);
    setStage(Voxel::ChunkVisibilityStage::GenerationWorkerFinish, 11);
    setStage(Voxel::ChunkVisibilityStage::GenerationReady, 12);
    setStage(Voxel::ChunkVisibilityStage::DataReady, 13);
    setStage(Voxel::ChunkVisibilityStage::NeighborReady, 14);
    setStage(Voxel::ChunkVisibilityStage::MeshEligible, 14);
    setStage(Voxel::ChunkVisibilityStage::SchedulerWait, 14);
    setStage(Voxel::ChunkVisibilityStage::PoolSubmit, 15);
    setStage(Voxel::ChunkVisibilityStage::WorkerStart, 16);
    setStage(Voxel::ChunkVisibilityStage::WorkerFinish, 18);
    setStage(Voxel::ChunkVisibilityStage::ResultAccepted, 19);

    auto converted = Benchmark::makeNearCameraVisibilitySample(record, 1);
    CHECK(converted.has_value());
    CHECK_EQ(converted->generationQueueWait, milliseconds(6));
    CHECK_EQ(converted->generationSchedulerWait, milliseconds(2));
    CHECK_EQ(converted->generationCapacityWait, milliseconds(2));
    CHECK_EQ(converted->generationPoolWait, milliseconds(2));
    CHECK_EQ(converted->dataReadyToNeighborsReady, milliseconds(0));
    CHECK_EQ(
        converted->dependencyReadyBoundary,
        Benchmark::DependencyReadyBoundary::InferredDataReady);
    CHECK_EQ(converted->neighborsReadyToMeshStart, milliseconds(3));

    record.origin = Voxel::ChunkVisibilityOrigin::Persisted;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
    record.origin = Voxel::ChunkVisibilityOrigin::Generated;
    record.outcome = Voxel::ChunkVisibilityOutcome::AcceptedEmptyGeometry;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
    record.outcome = Voxel::ChunkVisibilityOutcome::AcceptedNonemptyGeometry;
    setStage(Voxel::ChunkVisibilityStage::FirstDraw, 20);
    record.drawOutcome =
        Voxel::ChunkVisibilityDrawOutcome::MeshReplacedBeforeDraw;
    CHECK(!Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
    record.drawOutcome = Voxel::ChunkVisibilityDrawOutcome::Drawn;
    CHECK(Benchmark::makeNearCameraVisibilitySample(record, 1).has_value());
}
