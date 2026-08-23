#include "Rigel/Voxel/ChunkVisibilityTrace.h"

#include <algorithm>
#include <atomic>

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

bool hasPendingDraw(const ChunkVisibilityTraceRecord& record) {
    return (record.outcome ==
                ChunkVisibilityOutcome::CachedNonemptyGeometry ||
            record.outcome ==
                ChunkVisibilityOutcome::AcceptedNonemptyGeometry) &&
        !record.drawOutcome;
}

uint64_t nextLifecycleId() {
    static std::atomic<uint64_t> next{1};
    uint64_t id = 0;
    while (id == 0) {
        id = next.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
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
        case ChunkVisibilityOutcome::CachedEmptyGeometry:
            return "cached_empty_geometry";
        case ChunkVisibilityOutcome::CachedNonemptyGeometry:
            return "cached_nonempty_geometry";
        case ChunkVisibilityOutcome::AcceptedEmptyGeometry:
            return "accepted_empty_geometry";
        case ChunkVisibilityOutcome::AcceptedNonemptyGeometry:
            return "accepted_nonempty_geometry";
        case ChunkVisibilityOutcome::Stale:
            return "stale";
        case ChunkVisibilityOutcome::Failed:
            return "failed";
        case ChunkVisibilityOutcome::CameraLeft:
            return "camera_left";
        case ChunkVisibilityOutcome::TracerReplaced:
            return "tracer_replaced";
        case ChunkVisibilityOutcome::Reset:
            return "reset";
        case ChunkVisibilityOutcome::GeneratorReplaced:
            return "generator_replaced";
        case ChunkVisibilityOutcome::StreamerDestroyed:
            return "streamer_destroyed";
    }
    return "unknown";
}

std::string_view chunkVisibilityLifecycleKindName(
    ChunkVisibilityLifecycleKind kind) {
    switch (kind) {
        case ChunkVisibilityLifecycleKind::CameraDemand:
            return "camera_demand";
        case ChunkVisibilityLifecycleKind::Remesh:
            return "remesh";
    }
    return "unknown";
}

std::string_view chunkVisibilityDrawOutcomeName(
    ChunkVisibilityDrawOutcome outcome) {
    switch (outcome) {
        case ChunkVisibilityDrawOutcome::Drawn:
            return "drawn";
        case ChunkVisibilityDrawOutcome::CameraLeftBeforeDraw:
            return "camera_left_before_draw";
        case ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw:
            return "mesh_removed_before_draw";
        case ChunkVisibilityDrawOutcome::MeshReplacedBeforeDraw:
            return "mesh_replaced_before_draw";
        case ChunkVisibilityDrawOutcome::TraceReplacedBeforeDraw:
            return "trace_replaced_before_draw";
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

std::optional<ChunkVisibilityLifecycleKey> ChunkVisibilityTracer::begin(
    ChunkVisibilityLifecycleKind kind) {
    if (!enabled()) {
        return std::nullopt;
    }

    std::lock_guard lock(m_mutex);
    const ChunkVisibilityLifecycleKey key{
        m_config.coord,
        nextLifecycleId()
    };
    while (m_records.size() >= m_config.capacity) {
        ++m_droppedRecords;
        if (m_records.front().outcome == ChunkVisibilityOutcome::Pending ||
            hasPendingDraw(m_records.front())) {
            ++m_droppedUnfinishedRecords;
        }
        m_records.pop_front();
    }
    m_records.push_back({
        .key = key,
        .kind = kind
    });
    return key;
}

void ChunkVisibilityTracer::bindMeshTask(
    const ChunkVisibilityLifecycleKey& key,
    const ChunkVisibilityMeshTaskIdentity& meshTask) {
    if (!traces(key.coord)) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        return;
    }
    if (!record->meshTask) {
        record->meshTask = meshTask;
    }
}

void ChunkVisibilityTracer::mark(
    const ChunkVisibilityLifecycleKey& key,
    ChunkVisibilityStage stageValue) {
    mark(key, {stageValue});
}

void ChunkVisibilityTracer::mark(
    const ChunkVisibilityLifecycleKey& key,
    std::initializer_list<ChunkVisibilityStage> stageValues) {
    if (!traces(key.coord) || stageValues.size() == 0) {
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        auto record = findRecord(key);
        if (record == m_records.end()) {
            ++m_unmatchedEvents;
            return;
        }
        const bool needsTimestamp = std::any_of(
            stageValues.begin(), stageValues.end(),
            [&](ChunkVisibilityStage stage) {
                return stage != ChunkVisibilityStage::Count &&
                    stage != ChunkVisibilityStage::ResultAccepted &&
                    stage != ChunkVisibilityStage::FirstDraw &&
                    !record->stages[stageIndex(stage)];
            });
        if (!needsTimestamp) {
            return;
        }
    }

    const auto timestamp = now();
    if (!timestamp) {
        return;
    }
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        return;
    }
    for (ChunkVisibilityStage stage : stageValues) {
        if (stage == ChunkVisibilityStage::Count ||
            stage == ChunkVisibilityStage::ResultAccepted ||
            stage == ChunkVisibilityStage::FirstDraw) {
            continue;
        }
        auto& stageTime = record->stages[stageIndex(stage)];
        if (!stageTime) {
            stageTime = *timestamp;
        }
    }
}

void ChunkVisibilityTracer::complete(
    const ChunkVisibilityLifecycleKey& key,
    ChunkVisibilityOutcome outcome) {
    if (!traces(key.coord) || outcome == ChunkVisibilityOutcome::Pending) {
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        auto record = findRecord(key);
        if (record == m_records.end()) {
            ++m_unmatchedEvents;
            return;
        }
        if (record->outcome != ChunkVisibilityOutcome::Pending) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        return;
    }
    if (record->outcome != ChunkVisibilityOutcome::Pending) {
        return;
    }
    if (timestamp &&
        (outcome == ChunkVisibilityOutcome::AcceptedEmptyGeometry ||
         outcome == ChunkVisibilityOutcome::AcceptedNonemptyGeometry)) {
        record->stages[stageIndex(ChunkVisibilityStage::ResultAccepted)] =
            *timestamp;
    }
    record->terminalTime = timestamp;
    record->outcome = outcome;
}

void ChunkVisibilityTracer::observeDraw(
    const ChunkVisibilityLifecycleKey& key) {
    if (!traces(key.coord)) {
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        auto record = findRecord(key);
        if (record == m_records.end()) {
            ++m_unmatchedEvents;
            return;
        }
        if (!hasPendingDraw(*record)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        return;
    }
    if (!hasPendingDraw(*record)) {
        return;
    }
    if (timestamp) {
        record->stages[stageIndex(ChunkVisibilityStage::FirstDraw)] =
            *timestamp;
    }
    record->drawOutcome = ChunkVisibilityDrawOutcome::Drawn;
    record->drawTerminalTime = timestamp;
}

void ChunkVisibilityTracer::observeMeshUnavailable(
    const ChunkVisibilityLifecycleKey& key,
    ChunkVisibilityDrawOutcome outcome) {
    if (!traces(key.coord) || outcome == ChunkVisibilityDrawOutcome::Drawn) {
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        auto record = findRecord(key);
        if (record == m_records.end()) {
            ++m_unmatchedEvents;
            return;
        }
        if (!hasPendingDraw(*record)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        return;
    }
    if (hasPendingDraw(*record)) {
        record->drawOutcome = outcome;
        record->drawTerminalTime = timestamp;
    }
}

std::vector<ChunkVisibilityTraceRecord> ChunkVisibilityTracer::snapshot() const {
    if (!enabled()) {
        return {};
    }
    std::lock_guard lock(m_mutex);
    return {m_records.begin(), m_records.end()};
}

ChunkVisibilityTraceStats ChunkVisibilityTracer::stats() const {
    if (!enabled()) {
        return {};
    }
    std::lock_guard lock(m_mutex);
    return {
        .retainedRecords = m_records.size(),
        .droppedRecords = m_droppedRecords,
        .droppedUnfinishedRecords = m_droppedUnfinishedRecords,
        .unmatchedEvents = m_unmatchedEvents
    };
}

ChunkVisibilityTracer::RecordIterator ChunkVisibilityTracer::findRecord(
    const ChunkVisibilityLifecycleKey& key) {
    return std::find_if(
        m_records.begin(),
        m_records.end(),
        [&](const ChunkVisibilityTraceRecord& record) {
            return record.key == key;
        });
}

std::optional<ChunkVisibilityTimePoint> ChunkVisibilityTracer::now()
    const noexcept {
    try {
        std::lock_guard lock(m_clockMutex);
        return m_clock();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace Rigel::Voxel
