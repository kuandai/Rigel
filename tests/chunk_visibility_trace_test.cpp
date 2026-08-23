#include "TestFramework.h"

#include "Rigel/Voxel/ChunkVisibilityTrace.h"

#include <chrono>

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

ChunkVisibilityTraceIdentity identity(uint64_t requestId,
                                      uint64_t instanceId = 10,
                                      uint32_t revision = 4) {
    return {
        .coord = {1, 2, 3},
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
    const auto traceIdentity = identity(11);

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
    tracer.begin(traceIdentity, initial);

    clock.advance(std::chrono::milliseconds(5));
    tracer.mark(traceIdentity, ChunkVisibilityStage::PoolSubmit);
    clock.advance(std::chrono::milliseconds(6));
    tracer.mark(traceIdentity, ChunkVisibilityStage::WorkerStart);
    clock.advance(std::chrono::milliseconds(7));
    tracer.mark(traceIdentity, ChunkVisibilityStage::WorkerFinish);
    clock.advance(std::chrono::milliseconds(8));
    tracer.complete(
        traceIdentity,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    clock.advance(std::chrono::milliseconds(9));
    tracer.observeDraw(traceIdentity);

    const auto records = tracer.snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    const auto& record = records.front();
    CHECK_EQ(
        record.outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    for (size_t i = 0;
         i < static_cast<size_t>(ChunkVisibilityStage::Count);
         ++i) {
        CHECK(record.stages[i].has_value());
        if (i > 0) {
            CHECK(*record.stages[i] >= *record.stages[i - 1]);
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

TEST_CASE(ChunkVisibilityTrace_DistinguishesTerminalOutcomesAndRealDraw) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 8},
        [&]() { return clock.now(); });

    const std::array outcomes{
        ChunkVisibilityOutcome::VoxelEmpty,
        ChunkVisibilityOutcome::AcceptedEmptyGeometry,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry,
        ChunkVisibilityOutcome::Stale,
        ChunkVisibilityOutcome::Failed
    };
    for (size_t index = 0; index < outcomes.size(); ++index) {
        const auto traceIdentity = identity(index + 1);
        tracer.begin(traceIdentity);
        tracer.complete(traceIdentity, outcomes[index]);
        tracer.observeDraw(traceIdentity);
    }

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
        CHECK_EQ(
            records[index].stage(ChunkVisibilityStage::FirstDraw).has_value(),
            outcomes[index] ==
                ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    }
}

TEST_CASE(ChunkVisibilityTrace_StaleIdentityCannotCloseReplacement) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 4},
        [&]() { return clock.now(); });
    const auto stale = identity(20, 30, 1);
    const auto replacement = identity(21, 31, 2);

    tracer.begin(stale);
    tracer.begin(replacement);
    tracer.complete(stale, ChunkVisibilityOutcome::Stale);
    tracer.complete(
        stale,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    auto records = tracer.snapshot();
    CHECK_EQ(records[0].outcome, ChunkVisibilityOutcome::Stale);
    CHECK_EQ(records[1].outcome, ChunkVisibilityOutcome::Pending);

    tracer.complete(
        replacement,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    records = tracer.snapshot();
    CHECK_EQ(
        records[1].outcome,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
}

TEST_CASE(ChunkVisibilityTrace_BoundsRetentionIncludingPendingRecords) {
    ChunkVisibilityTracer tracer({{1, 2, 3}, 2});
    tracer.begin(identity(1));
    tracer.begin(identity(2));
    tracer.begin(identity(3));

    const auto records = tracer.snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK_EQ(records[0].identity.requestId, static_cast<uint64_t>(2));
    CHECK_EQ(records[1].identity.requestId, static_cast<uint64_t>(3));
}

TEST_CASE(ChunkVisibilityTrace_DisabledDoesNotReadClockOrRetainRecords) {
    ManualClock clock;
    ChunkVisibilityTracer tracer(
        {{1, 2, 3}, 0},
        [&]() { return clock.now(); });
    const auto traceIdentity = identity(1);

    CHECK(!tracer.capture().has_value());
    tracer.begin(traceIdentity);
    tracer.mark(traceIdentity, ChunkVisibilityStage::WorkerStart);
    tracer.complete(traceIdentity, ChunkVisibilityOutcome::Failed);
    tracer.observeDraw(traceIdentity);

    CHECK_EQ(clock.reads(), static_cast<size_t>(0));
    CHECK(tracer.snapshot().empty());
}
