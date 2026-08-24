#include "TestFramework.h"

#include "Rigel/Voxel/ChunkVisibilityTrace.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace Rigel::Voxel;

namespace {
class ManualClock {
public:
    ChunkVisibilityTimePoint now() {
        ++m_reads;
        return m_now;
    }

    void advance(std::chrono::milliseconds amount) {
        m_now += amount;
    }

    size_t reads() const { return m_reads; }

private:
    ChunkVisibilityTimePoint m_now{};
    size_t m_reads = 0;
};

ChunkVisibilityLifecycleKey key(uint64_t lifecycleId) {
    return {{1, 2, 3}, lifecycleId};
}

ChunkVisibilityMeshTaskIdentity meshTask(
    uint64_t requestId,
    uint64_t instanceId = 10,
    uint32_t revision = 4) {
    return {
        .requestId = requestId,
        .workEpoch = 7,
        .chunkInstanceId = instanceId,
        .revision = revision
    };
}

long long milliseconds(const std::optional<ChunkVisibilityDuration>& duration) {
    CHECK(duration.has_value());
    return std::chrono::duration_cast<std::chrono::milliseconds>(*duration)
        .count();
}
} // namespace

TEST_CASE(ChunkVisibilityTrace_CapturesOrderedStagesAndDerivedDurations) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 4},
        [&]() { return clock.now(); });
    const auto lifecycleKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);

    tracer.mark(lifecycleKey, ChunkVisibilityStage::Desired);
    clock.advance(std::chrono::milliseconds(1));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::DataRequest);
    clock.advance(std::chrono::milliseconds(2));
    tracer.mark(
        lifecycleKey,
        ChunkVisibilityStage::GenerationSchedulerPending);
    clock.advance(std::chrono::milliseconds(3));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::GenerationCapacityWait);
    clock.advance(std::chrono::milliseconds(4));
    tracer.mark(
        lifecycleKey, ChunkVisibilityStage::GenerationExecutorSubmit);
    clock.advance(std::chrono::milliseconds(5));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::GenerationWorkerStart);
    clock.advance(std::chrono::milliseconds(6));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::GenerationWorkerFinish);
    clock.advance(std::chrono::milliseconds(1));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::GenerationReady);
    clock.advance(std::chrono::milliseconds(2));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::DataReady);
    tracer.observeMissingDesiredCardinalNeighborCount(lifecycleKey, 6);
    tracer.observeMissingDesiredCardinalNeighborCount(lifecycleKey, 0);
    ChunkVisibilityBlockingNeighborSnapshot firstBlockers;
    firstBlockers.count = 1;
    firstBlockers.neighbors[0] = {
        Direction::PosX,
        {2, 2, 3},
        true,
        ChunkVisibilityBlockerState::GenerationExecutorQueued};
    tracer.observeBlockingDesiredCardinalNeighbors(
        lifecycleKey, firstBlockers);
    ChunkVisibilityBlockingNeighborSnapshot currentBlockers;
    currentBlockers.count = 1;
    currentBlockers.neighbors[0] = {
        Direction::NegX,
        {0, 2, 3},
        true,
        ChunkVisibilityBlockerState::ReadyResident};
    tracer.observeBlockingDesiredCardinalNeighbors(
        lifecycleKey, currentBlockers);
    clock.advance(std::chrono::milliseconds(3));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::NeighborReady);
    clock.advance(std::chrono::milliseconds(4));
    tracer.mark(
        lifecycleKey,
        {
            ChunkVisibilityStage::MeshEligible,
            ChunkVisibilityStage::SchedulerWait
        });
    tracer.bindMeshTask(lifecycleKey, meshTask(99));

    clock.advance(std::chrono::milliseconds(5));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::PoolSubmit);
    clock.advance(std::chrono::milliseconds(6));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::WorkerStart);
    clock.advance(std::chrono::milliseconds(7));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::WorkerFinish);
    clock.advance(std::chrono::milliseconds(8));
    tracer.complete(
        lifecycleKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    clock.advance(std::chrono::milliseconds(9));
    tracer.observeDraw(lifecycleKey);

    const auto records = tracer.snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    const auto& record = records.front();
    CHECK_EQ(record.key, lifecycleKey);
    CHECK_EQ(record.kind, ChunkVisibilityLifecycleKind::CameraDemand);
    CHECK(record.meshTask.has_value());
    CHECK_EQ(*record.meshTask, meshTask(99));
    CHECK_EQ(
        record.firstObservedMissingDesiredCardinalNeighborCount,
        std::optional<uint8_t>{6});
    CHECK_EQ(
        record.firstObservedBlockingDesiredCardinalNeighbors,
        firstBlockers);
    CHECK_EQ(record.blockingDesiredCardinalNeighbors, currentBlockers);
    CHECK_EQ(
        tracer.blockingDesiredCardinalNeighbors(lifecycleKey),
        currentBlockers);
    CHECK_EQ(
        record.outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_EQ(record.drawOutcome, ChunkVisibilityDrawOutcome::Drawn);
    for (size_t index = 0;
         index < static_cast<size_t>(ChunkVisibilityStage::Count);
         ++index) {
        CHECK(record.stages[index].has_value());
        if (index > 0) {
            CHECK(*record.stages[index] >= *record.stages[index - 1]);
        }
    }

    const auto durations = record.durations();
    CHECK_EQ(milliseconds(durations.desiredToDataRequest), 1);
    CHECK_EQ(milliseconds(durations.dataWait), 23);
    CHECK_EQ(milliseconds(durations.generationQueueWait), 3);
    CHECK_EQ(milliseconds(durations.generationCapacityWait), 4);
    CHECK_EQ(milliseconds(durations.generationPoolWait), 5);
    CHECK_EQ(milliseconds(durations.generationExecution), 6);
    CHECK_EQ(milliseconds(durations.generationResultWait), 2);
    CHECK_EQ(milliseconds(durations.dependencyWait), 3);
    CHECK_EQ(milliseconds(durations.eligibilityWait), 4);
    CHECK_EQ(milliseconds(durations.eligibleToWorkerStart), 11);
    CHECK_EQ(milliseconds(durations.schedulerWait), 5);
    CHECK_EQ(milliseconds(durations.poolWait), 6);
    CHECK_EQ(milliseconds(durations.workerExecution), 7);
    CHECK_EQ(milliseconds(durations.resultWait), 8);
    CHECK_EQ(milliseconds(durations.desiredToAccepted), 57);
    CHECK_EQ(milliseconds(durations.desiredToFirstDraw), 66);
}

TEST_CASE(ChunkVisibilityTrace_DistinguishesBuildAndDrawOutcomes) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 8},
        [&]() { return clock.now(); });

    const std::array outcomes{
        ChunkVisibilityOutcome::VoxelEmpty,
        ChunkVisibilityOutcome::CachedEmptyGeometry,
        ChunkVisibilityOutcome::CachedNonemptyGeometry,
        ChunkVisibilityOutcome::AcceptedEmptyGeometry,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry,
        ChunkVisibilityOutcome::Stale,
        ChunkVisibilityOutcome::Failed
    };
    std::vector<ChunkVisibilityLifecycleKey> lifecycleKeys;
    for (size_t index = 0; index < outcomes.size(); ++index) {
        const auto lifecycleKey = *tracer.begin(
            index < 3
                ? ChunkVisibilityLifecycleKind::CameraDemand
                : ChunkVisibilityLifecycleKind::Remesh);
        lifecycleKeys.push_back(lifecycleKey);
        if (index >= 3) {
            tracer.bindMeshTask(lifecycleKey, meshTask(index + 10));
        }
        tracer.complete(lifecycleKey, outcomes[index]);
    }
    tracer.observeDraw(lifecycleKeys[2]);
    tracer.observeMeshUnavailable(
        lifecycleKeys[4],
        ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);

    const auto records = tracer.snapshot();
    CHECK_EQ(records.size(), outcomes.size());
    for (size_t index = 0; index < outcomes.size(); ++index) {
        CHECK_EQ(records[index].outcome, outcomes[index]);
        const bool accepted =
            outcomes[index] ==
                ChunkVisibilityOutcome::AcceptedEmptyGeometry ||
            outcomes[index] ==
                ChunkVisibilityOutcome::AcceptedNonemptyGeometry;
        CHECK_EQ(
            records[index]
                .stage(ChunkVisibilityStage::ResultAccepted)
                .has_value(),
            accepted);
        CHECK_EQ(records[index].meshTask.has_value(), index >= 3);
    }
    CHECK_EQ(records[2].drawOutcome, ChunkVisibilityDrawOutcome::Drawn);
    CHECK_EQ(
        records[4].drawOutcome,
        ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
    CHECK(!records[4].stage(ChunkVisibilityStage::FirstDraw).has_value());
}

TEST_CASE(ChunkVisibilityTrace_GenerationQueueWaitEndsAtExecutorWithoutCapacityWait) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 1},
        [&]() { return clock.now(); });
    const auto lifecycleKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);

    tracer.mark(
        lifecycleKey, ChunkVisibilityStage::GenerationSchedulerPending);
    clock.advance(std::chrono::milliseconds(7));
    tracer.mark(
        lifecycleKey, ChunkVisibilityStage::GenerationExecutorSubmit);

    const auto record = tracer.latestRecord();
    CHECK(record.has_value());
    const auto durations = record->durations();
    CHECK_EQ(milliseconds(durations.generationQueueWait), 7);
    CHECK(!durations.generationCapacityWait.has_value());
}

TEST_CASE(ChunkVisibilityTrace_TerminalLifecycleRejectsLateMutations) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 4},
        [&]() { return clock.now(); });
    const std::array terminalOutcomes{
        ChunkVisibilityOutcome::CameraLeft,
        ChunkVisibilityOutcome::Reset,
        ChunkVisibilityOutcome::GeneratorReplaced,
        ChunkVisibilityOutcome::TracerReplaced};

    for (const auto outcome : terminalOutcomes) {
        const auto lifecycleKey = *tracer.begin(
            ChunkVisibilityLifecycleKind::CameraDemand);
        tracer.mark(lifecycleKey, ChunkVisibilityStage::Desired);
        tracer.complete(lifecycleKey, outcome);
        const auto terminalMeasurement = tracer.measurement();
        const auto terminal = terminalMeasurement.records.back();
        const size_t terminalClockReads = clock.reads();

        ChunkVisibilityBlockingNeighborSnapshot blockers;
        blockers.count = 1;
        blockers.neighbors[0] = {
            Direction::PosX,
            {2, 2, 3},
            true,
            ChunkVisibilityBlockerState::GenerationWorkerRunning};
        tracer.bindMeshTask(lifecycleKey, meshTask(400));
        tracer.markDataReady(
            lifecycleKey, ChunkVisibilityOrigin::Generated);
        tracer.observeMissingDesiredCardinalNeighborCount(lifecycleKey, 1);
        tracer.observeBlockingDesiredCardinalNeighbors(
            lifecycleKey, blockers);
        tracer.mark(
            lifecycleKey,
            {
                ChunkVisibilityStage::GenerationWorkerStart,
                ChunkVisibilityStage::GenerationWorkerFinish,
                ChunkVisibilityStage::GenerationReady
            });
        tracer.complete(
            lifecycleKey,
            ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
        tracer.observeDraw(lifecycleKey);
        tracer.observeMeshUnavailable(
            lifecycleKey,
            ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);

        const auto after = tracer.measurement();
        const auto& retained = after.records.back();
        CHECK_EQ(after.sequence, terminalMeasurement.sequence);
        CHECK_EQ(clock.reads(), terminalClockReads);
        CHECK_EQ(retained.outcome, outcome);
        CHECK_EQ(retained.origin, terminal.origin);
        CHECK_EQ(retained.meshTask, terminal.meshTask);
        CHECK_EQ(retained.stages, terminal.stages);
        CHECK_EQ(retained.observedStages, terminal.observedStages);
        CHECK_EQ(
            retained.firstObservedMissingDesiredCardinalNeighborCount,
            terminal.firstObservedMissingDesiredCardinalNeighborCount);
        CHECK_EQ(
            retained.firstObservedBlockingDesiredCardinalNeighbors,
            terminal.firstObservedBlockingDesiredCardinalNeighbors);
        CHECK_EQ(
            retained.blockingDesiredCardinalNeighbors,
            terminal.blockingDesiredCardinalNeighbors);
        CHECK_EQ(retained.terminalTime, terminal.terminalTime);
        CHECK_EQ(retained.drawOutcome, terminal.drawOutcome);
        CHECK_EQ(retained.drawTerminalTime, terminal.drawTerminalTime);
    }
}

TEST_CASE(ChunkVisibilityTrace_LifecycleKeyIsolatesReplacementTask) {
    ChunkVisibilityTracer tracer({{1, 2, 3}, 4});
    const auto staleKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    const auto replacementKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::Remesh);
    const auto staleTask = meshTask(30, 40, 1);
    const auto replacementTask = meshTask(31, 40, 2);

    tracer.bindMeshTask(staleKey, staleTask);
    tracer.bindMeshTask(replacementKey, replacementTask);
    tracer.complete(staleKey, ChunkVisibilityOutcome::Stale);
    tracer.complete(
        staleKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    auto records = tracer.snapshot();
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Stale);
    CHECK_EQ(records[0].meshTask, staleTask);
    CHECK_EQ(records[1].outcome, ChunkVisibilityOutcome::Pending);
    CHECK_EQ(records[1].meshTask, replacementTask);

    tracer.complete(
        replacementKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    records = tracer.snapshot();
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
}

TEST_CASE(ChunkVisibilityTrace_BoundsRetentionAndAccountsDroppedWork) {
    ChunkVisibilityTracer tracer({{1, 2, 3}, 2});
    const auto firstKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    tracer.complete(
        firstKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    const auto secondKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::Remesh);
    const auto thirdKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::Remesh);

    auto records = tracer.snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].key, secondKey);
    CHECK_EQ(records[1].key, thirdKey);
    auto stats = tracer.measurement().stats;
    CHECK_EQ(stats.retainedRecords, static_cast<size_t>(2));
    CHECK_EQ(stats.droppedRecords, static_cast<uint64_t>(1));
    CHECK_EQ(stats.droppedUnfinishedRecords, static_cast<uint64_t>(1));

    tracer.observeDraw(firstKey);
    stats = tracer.measurement().stats;
    CHECK_EQ(stats.unmatchedEvents, static_cast<uint64_t>(1));

    const auto measurement = tracer.measurement();
    CHECK(measurement.sequence > 0);
    CHECK_EQ(measurement.records.size(), tracer.snapshot().size());
    CHECK_EQ(
        measurement.stats.retainedRecords,
        measurement.records.size());
    CHECK_EQ(
        measurement.stats.droppedRecords,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        measurement.stats.droppedUnfinishedRecords,
        static_cast<uint64_t>(1));
    CHECK_EQ(
        measurement.stats.unmatchedEvents,
        static_cast<uint64_t>(1));
}

TEST_CASE(ChunkVisibilityTrace_ClassifiesObservedAndCensoredOrigins) {
    ChunkVisibilityTracer tracer({{1, 2, 3}, 3});
    const auto persisted = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    tracer.markDataReady(persisted, ChunkVisibilityOrigin::Persisted);
    const auto resident = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand,
        ChunkVisibilityOrigin::ResidentLeftCensored);
    const auto remesh = *tracer.begin(
        ChunkVisibilityLifecycleKind::Remesh,
        ChunkVisibilityOrigin::Remesh);

    const auto measurement = tracer.measurement();
    CHECK_EQ(measurement.records.size(), static_cast<size_t>(3));
    CHECK_EQ(
        measurement.records[0].origin,
        ChunkVisibilityOrigin::Persisted);
    CHECK(measurement.records[0]
              .stage(ChunkVisibilityStage::DataReady)
              .has_value());
    CHECK_EQ(
        measurement.records[1].origin,
        ChunkVisibilityOrigin::ResidentLeftCensored);
    CHECK(!measurement.records[1]
               .stage(ChunkVisibilityStage::DataReady)
               .has_value());
    CHECK_EQ(
        measurement.records[2].origin,
        ChunkVisibilityOrigin::Remesh);
    CHECK_EQ(
        chunkVisibilityOriginName(ChunkVisibilityOrigin::Generated),
        std::string_view("generated"));
    CHECK(measurement.sequence > 0);
}

TEST_CASE(ChunkVisibilityTrace_CustomClockRunsSerializedWithoutRecordLock) {
    std::shared_ptr<ChunkVisibilityTracer> tracer;
    std::atomic<int> activeClockCalls{0};
    std::atomic<bool> concurrentClockCall{false};
    std::atomic<bool> recordMutexWasFree{true};
    ChunkVisibilityTimePoint now{};
    tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{{1, 2, 3}, 2},
        [&]() {
            if (activeClockCalls.fetch_add(1, std::memory_order_acq_rel) != 0) {
                concurrentClockCall.store(true, std::memory_order_relaxed);
            }
            auto snapshot = std::async(std::launch::async, [&]() {
                return tracer->snapshot().size();
            });
            if (snapshot.wait_for(std::chrono::seconds(1)) !=
                std::future_status::ready) {
                recordMutexWasFree.store(false, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const auto result = now;
            now += std::chrono::milliseconds(1);
            activeClockCalls.fetch_sub(1, std::memory_order_acq_rel);
            return result;
        });
    const auto firstKey = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    const auto secondKey = *tracer->begin(
        ChunkVisibilityLifecycleKind::Remesh);

    std::thread first([&]() {
        tracer->mark(firstKey, ChunkVisibilityStage::WorkerStart);
    });
    std::thread second([&]() {
        tracer->mark(secondKey, ChunkVisibilityStage::WorkerStart);
    });
    first.join();
    second.join();

    CHECK(recordMutexWasFree.load(std::memory_order_relaxed));
    CHECK(!concurrentClockCall.load(std::memory_order_relaxed));
}

TEST_CASE(ChunkVisibilityTrace_ClockFailureCannotEscapeOrHoldLifecycleOpen) {
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 1},
        []() -> ChunkVisibilityTimePoint {
            throw std::runtime_error("clock failure");
        });
    const auto lifecycleKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);

    CHECK_NO_THROW(
        tracer.mark(lifecycleKey, ChunkVisibilityStage::WorkerStart));
    CHECK_NO_THROW(tracer.complete(
        lifecycleKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry));
    CHECK_NO_THROW(tracer.observeDraw(lifecycleKey));

    const auto records = tracer.snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    CHECK_EQ(
        records.front().drawOutcome,
        ChunkVisibilityDrawOutcome::Drawn);
    CHECK(!records.front().terminalTime.has_value());
    CHECK(!records.front().drawTerminalTime.has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::WorkerStart).has_value());
    CHECK(!records.front()
               .stage(ChunkVisibilityStage::ResultAccepted)
               .has_value());
    CHECK(!records.front().stage(ChunkVisibilityStage::FirstDraw).has_value());
    const auto measurement = tracer.measurement();
    CHECK_EQ(measurement.stats.clockFailures, static_cast<uint64_t>(3));
    CHECK_EQ(
        measurement.stats.retainedRecords,
        measurement.records.size());
}

TEST_CASE(ChunkVisibilityTrace_ClockFailureDoesNotRetimestampTransition) {
    size_t reads = 0;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 1},
        [&]() {
            ++reads;
            if (reads == 1) {
                throw std::runtime_error("first read fails");
            }
            return ChunkVisibilityTimePoint{} + std::chrono::seconds(1);
        });
    const auto lifecycleKey = *tracer.begin(
        ChunkVisibilityLifecycleKind::CameraDemand);

    tracer.mark(lifecycleKey, ChunkVisibilityStage::MeshEligible);
    tracer.mark(lifecycleKey, ChunkVisibilityStage::MeshEligible);

    const auto measurement = tracer.measurement();
    CHECK_EQ(reads, static_cast<size_t>(1));
    CHECK_EQ(measurement.stats.clockFailures, static_cast<uint64_t>(1));
    CHECK(measurement.records.front().observed(
        ChunkVisibilityStage::MeshEligible));
    CHECK(!measurement.records.front()
               .stage(ChunkVisibilityStage::MeshEligible)
               .has_value());
}

TEST_CASE(ChunkVisibilityTrace_DisabledDoesNotReadClockOrRetainRecords) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 0},
        [&]() { return clock.now(); });
    const auto lifecycleKey = key(1);

    CHECK(!tracer.begin(ChunkVisibilityLifecycleKind::CameraDemand));
    tracer.bindMeshTask(lifecycleKey, meshTask(1));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::WorkerStart);
    tracer.complete(lifecycleKey, ChunkVisibilityOutcome::Failed);
    tracer.observeDraw(lifecycleKey);

    CHECK_EQ(clock.reads(), static_cast<size_t>(0));
    CHECK(tracer.snapshot().empty());
    CHECK_EQ(
        tracer.measurement().stats.retainedRecords,
        static_cast<size_t>(0));
    const auto measurement = tracer.measurement();
    CHECK_EQ(measurement.sequence, static_cast<uint64_t>(0));
    CHECK(measurement.records.empty());
    CHECK_EQ(measurement.stats.clockFailures, static_cast<uint64_t>(0));
}
