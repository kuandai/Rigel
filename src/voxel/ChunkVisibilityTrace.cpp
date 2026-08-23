#include "Rigel/Voxel/ChunkVisibilityTrace.h"

#include <algorithm>

namespace Rigel::Voxel {

namespace {
size_t stageIndex(ChunkVisibilityStage stage) {
    return static_cast<size_t>(stage);
}

std::optional<ChunkVisibilityDuration> between(
    const ChunkVisibilityStageTimes& stages,
    ChunkVisibilityStage from,
    ChunkVisibilityStage to) {
    const auto& start = stages[stageIndex(from)];
    const auto& finish = stages[stageIndex(to)];
    if (!start || !finish || *finish < *start) {
        return std::nullopt;
    }
    return *finish - *start;
}
} // namespace

std::string_view chunkVisibilityStageName(ChunkVisibilityStage stage) {
    switch (stage) {
        case ChunkVisibilityStage::Desired:
            return "desired";
        case ChunkVisibilityStage::DataRequest:
            return "data_request";
        case ChunkVisibilityStage::DataReady:
            return "data_ready";
        case ChunkVisibilityStage::NeighborReady:
            return "neighbor_ready";
        case ChunkVisibilityStage::MeshEligible:
            return "mesh_eligible";
        case ChunkVisibilityStage::SchedulerWait:
            return "scheduler_wait";
        case ChunkVisibilityStage::PoolSubmit:
            return "pool_submit";
        case ChunkVisibilityStage::WorkerStart:
            return "worker_start";
        case ChunkVisibilityStage::WorkerFinish:
            return "worker_finish";
        case ChunkVisibilityStage::ResultAccepted:
            return "result_accepted";
        case ChunkVisibilityStage::FirstDraw:
            return "first_draw";
        case ChunkVisibilityStage::Count:
            break;
    }
    return "unknown";
}

std::string_view chunkVisibilityOutcomeName(ChunkVisibilityOutcome outcome) {
    switch (outcome) {
        case ChunkVisibilityOutcome::Pending:
            return "pending";
        case ChunkVisibilityOutcome::VoxelEmpty:
            return "voxel_empty";
        case ChunkVisibilityOutcome::AcceptedEmptyGeometry:
            return "accepted_empty_geometry";
        case ChunkVisibilityOutcome::AcceptedNonemptyGeometry:
            return "accepted_nonempty_geometry";
        case ChunkVisibilityOutcome::Stale:
            return "stale";
        case ChunkVisibilityOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

std::optional<ChunkVisibilityTimePoint> ChunkVisibilityTraceRecord::stage(
    ChunkVisibilityStage value) const {
    if (value == ChunkVisibilityStage::Count) {
        return std::nullopt;
    }
    return stages[stageIndex(value)];
}

ChunkVisibilityDurations ChunkVisibilityTraceRecord::durations() const {
    return {
        .desiredToDataRequest = between(
            stages,
            ChunkVisibilityStage::Desired,
            ChunkVisibilityStage::DataRequest),
        .dataWait = between(
            stages,
            ChunkVisibilityStage::DataRequest,
            ChunkVisibilityStage::DataReady),
        .dependencyWait = between(
            stages,
            ChunkVisibilityStage::DataReady,
            ChunkVisibilityStage::NeighborReady),
        .eligibilityWait = between(
            stages,
            ChunkVisibilityStage::NeighborReady,
            ChunkVisibilityStage::MeshEligible),
        .schedulerWait = between(
            stages,
            ChunkVisibilityStage::SchedulerWait,
            ChunkVisibilityStage::PoolSubmit),
        .poolWait = between(
            stages,
            ChunkVisibilityStage::PoolSubmit,
            ChunkVisibilityStage::WorkerStart),
        .workerExecution = between(
            stages,
            ChunkVisibilityStage::WorkerStart,
            ChunkVisibilityStage::WorkerFinish),
        .resultWait = between(
            stages,
            ChunkVisibilityStage::WorkerFinish,
            ChunkVisibilityStage::ResultAccepted),
        .desiredToAccepted = between(
            stages,
            ChunkVisibilityStage::Desired,
            ChunkVisibilityStage::ResultAccepted),
        .desiredToFirstDraw = between(
            stages,
            ChunkVisibilityStage::Desired,
            ChunkVisibilityStage::FirstDraw)
    };
}

ChunkVisibilityTracer::ChunkVisibilityTracer(Config config, Clock clock)
    : m_config(config),
      m_clock(clock ? std::move(clock) : []() {
          return ChunkVisibilityClock::now();
      }) {}

std::optional<ChunkVisibilityTimePoint> ChunkVisibilityTracer::capture() const {
    if (!enabled()) {
        return std::nullopt;
    }
    return now();
}

void ChunkVisibilityTracer::begin(
    const ChunkVisibilityTraceIdentity& identity,
    ChunkVisibilityStageTimes initialStages) {
    if (!traces(identity.coord)) {
        return;
    }

    std::lock_guard lock(m_mutex);
    if (findRecord(identity) != m_records.end()) {
        return;
    }
    while (m_records.size() >= m_config.capacity) {
        m_records.pop_front();
    }
    m_records.push_back({
        .identity = identity,
        .stages = std::move(initialStages)
    });
}

void ChunkVisibilityTracer::mark(
    const ChunkVisibilityTraceIdentity& identity,
    ChunkVisibilityStage stageValue) {
    if (!traces(identity.coord) || stageValue == ChunkVisibilityStage::Count) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto record = findRecord(identity);
    if (record == m_records.end()) {
        return;
    }
    auto& timestamp = record->stages[stageIndex(stageValue)];
    if (!timestamp) {
        timestamp = now();
    }
}

void ChunkVisibilityTracer::complete(
    const ChunkVisibilityTraceIdentity& identity,
    ChunkVisibilityOutcome outcome) {
    if (!traces(identity.coord) || outcome == ChunkVisibilityOutcome::Pending) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto record = findRecord(identity);
    if (record == m_records.end() ||
        record->outcome != ChunkVisibilityOutcome::Pending) {
        return;
    }
    const auto timestamp = now();
    if (outcome == ChunkVisibilityOutcome::AcceptedEmptyGeometry ||
        outcome == ChunkVisibilityOutcome::AcceptedNonemptyGeometry) {
        record->stages[stageIndex(ChunkVisibilityStage::ResultAccepted)] =
            timestamp;
    }
    record->terminalTime = timestamp;
    record->outcome = outcome;
}

void ChunkVisibilityTracer::observeDraw(
    const ChunkVisibilityTraceIdentity& identity) {
    if (!traces(identity.coord)) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto record = findRecord(identity);
    if (record == m_records.end() ||
        record->outcome !=
            ChunkVisibilityOutcome::AcceptedNonemptyGeometry) {
        return;
    }
    auto& firstDraw = record->stages[stageIndex(ChunkVisibilityStage::FirstDraw)];
    if (!firstDraw) {
        firstDraw = now();
    }
}

std::vector<ChunkVisibilityTraceRecord> ChunkVisibilityTracer::snapshot() const {
    if (!enabled()) {
        return {};
    }
    std::lock_guard lock(m_mutex);
    return {m_records.begin(), m_records.end()};
}

ChunkVisibilityTracer::RecordIterator ChunkVisibilityTracer::findRecord(
    const ChunkVisibilityTraceIdentity& identity) {
    return std::find_if(
        m_records.begin(),
        m_records.end(),
        [&](const ChunkVisibilityTraceRecord& record) {
            return record.identity == identity;
        });
}

ChunkVisibilityTimePoint ChunkVisibilityTracer::now() const {
    return m_clock();
}

} // namespace Rigel::Voxel
