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
    const auto lifecycleKey = key(11);

    ChunkVisibilityStageTimes initial{};
    initial[static_cast<size_t>(ChunkVisibilityStage::Desired)] =
        *tracer.capture();
    clock.advance(std::chrono::milliseconds(1));
    initial[static_cast<size_t>(ChunkVisibilityStage::DataRequest)] =
        *tracer.capture();
    clock.advance(std::chrono::milliseconds(2));
    initial[static_cast<size_t>(ChunkVisibilityStage::DataReady)] =
        *tracer.capture();
    clock.advance(std::chrono::milliseconds(3));
    initial[static_cast<size_t>(ChunkVisibilityStage::NeighborReady)] =
        *tracer.capture();
    clock.advance(std::chrono::milliseconds(4));
    initial[static_cast<size_t>(ChunkVisibilityStage::MeshEligible)] =
        *tracer.capture();
    initial[static_cast<size_t>(ChunkVisibilityStage::SchedulerWait)] =
        initial[static_cast<size_t>(ChunkVisibilityStage::MeshEligible)];
    tracer.begin(
        lifecycleKey,
        ChunkVisibilityLifecycleKind::CameraDemand,
        initial);
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
    CHECK_EQ(milliseconds(durations.dataWait), 2);
    CHECK_EQ(milliseconds(durations.dependencyWait), 3);
    CHECK_EQ(milliseconds(durations.eligibilityWait), 4);
    CHECK_EQ(milliseconds(durations.schedulerWait), 5);
    CHECK_EQ(milliseconds(durations.poolWait), 6);
    CHECK_EQ(milliseconds(durations.workerExecution), 7);
    CHECK_EQ(milliseconds(durations.resultWait), 8);
    CHECK_EQ(milliseconds(durations.desiredToAccepted), 36);
    CHECK_EQ(milliseconds(durations.desiredToFirstDraw), 45);
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
    for (size_t index = 0; index < outcomes.size(); ++index) {
        const auto lifecycleKey = key(index + 1);
        tracer.begin(
            lifecycleKey,
            index < 3
                ? ChunkVisibilityLifecycleKind::CameraDemand
                : ChunkVisibilityLifecycleKind::Remesh);
        if (index >= 3) {
            tracer.bindMeshTask(lifecycleKey, meshTask(index + 10));
        }
        tracer.complete(lifecycleKey, outcomes[index]);
    }
    tracer.observeDraw(key(3));
    tracer.observeMeshUnavailable(
        key(5),
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

TEST_CASE(ChunkVisibilityTrace_LifecycleKeyIsolatesReplacementTask) {
    ChunkVisibilityTracer tracer({{1, 2, 3}, 4});
    const auto staleKey = key(20);
    const auto replacementKey = key(21);
    const auto staleTask = meshTask(30, 40, 1);
    const auto replacementTask = meshTask(31, 40, 2);

    tracer.begin(staleKey, ChunkVisibilityLifecycleKind::CameraDemand);
    tracer.bindMeshTask(staleKey, staleTask);
    tracer.begin(replacementKey, ChunkVisibilityLifecycleKind::Remesh);
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
    tracer.begin(key(1), ChunkVisibilityLifecycleKind::CameraDemand);
    tracer.complete(
        key(1),
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    tracer.begin(key(2), ChunkVisibilityLifecycleKind::Remesh);
    tracer.begin(key(3), ChunkVisibilityLifecycleKind::Remesh);

    auto records = tracer.snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].key, key(2));
    CHECK_EQ(records[1].key, key(3));
    auto stats = tracer.stats();
    CHECK_EQ(stats.retainedRecords, static_cast<size_t>(2));
    CHECK_EQ(stats.droppedRecords, static_cast<uint64_t>(1));
    CHECK_EQ(stats.droppedUnfinishedRecords, static_cast<uint64_t>(1));

    tracer.observeDraw(key(1));
    stats = tracer.stats();
    CHECK_EQ(stats.unmatchedEvents, static_cast<uint64_t>(1));
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
    tracer->begin(key(1), ChunkVisibilityLifecycleKind::CameraDemand);
    tracer->begin(key(2), ChunkVisibilityLifecycleKind::Remesh);

    std::thread first([&]() {
        tracer->mark(key(1), ChunkVisibilityStage::WorkerStart);
    });
    std::thread second([&]() {
        tracer->mark(key(2), ChunkVisibilityStage::WorkerStart);
    });
    first.join();
    second.join();

    CHECK(recordMutexWasFree.load(std::memory_order_relaxed));
    CHECK(!concurrentClockCall.load(std::memory_order_relaxed));
}

TEST_CASE(ChunkVisibilityTrace_DisabledDoesNotReadClockOrRetainRecords) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 0},
        [&]() { return clock.now(); });
    const auto lifecycleKey = key(1);

    CHECK(!tracer.capture().has_value());
    tracer.begin(
        lifecycleKey,
        ChunkVisibilityLifecycleKind::CameraDemand);
    tracer.bindMeshTask(lifecycleKey, meshTask(1));
    tracer.mark(lifecycleKey, ChunkVisibilityStage::WorkerStart);
    tracer.complete(lifecycleKey, ChunkVisibilityOutcome::Failed);
    tracer.observeDraw(lifecycleKey);

    CHECK_EQ(clock.reads(), static_cast<size_t>(0));
    CHECK(tracer.snapshot().empty());
    CHECK_EQ(tracer.stats().retainedRecords, static_cast<size_t>(0));
}
