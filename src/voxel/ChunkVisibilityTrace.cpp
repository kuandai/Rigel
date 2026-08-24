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

bool canRecordDrawTransition(const ChunkVisibilityTraceRecord& record) {
    // The mesh store publishes its trace link immediately before the streamer
    // commits the corresponding nonempty outcome.
    return !record.drawOutcome &&
        (record.outcome == ChunkVisibilityOutcome::Pending ||
         hasPendingDraw(record));
}

uint64_t nextLifecycleId() {
    static std::atomic<uint64_t> next{1};
    uint64_t id = 0;
    while (id == 0) {
        id = next.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

void advanceSequence(uint64_t& sequence) {
    ++sequence;
    if (sequence == 0) {
        ++sequence;
    }
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

std::string_view chunkVisibilityOriginName(ChunkVisibilityOrigin origin) {
    switch (origin) {
        case ChunkVisibilityOrigin::Unresolved:
            return "unresolved";
        case ChunkVisibilityOrigin::ResidentLeftCensored:
            return "resident_left_censored";
        case ChunkVisibilityOrigin::Persisted:
            return "persisted";
        case ChunkVisibilityOrigin::Generated:
            return "generated";
        case ChunkVisibilityOrigin::Remesh:
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

bool ChunkVisibilityTraceRecord::observed(
    ChunkVisibilityStage value) const {
    return value != ChunkVisibilityStage::Count &&
        observedStages[stageIndex(value)];
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
        .eligibleToWorkerStart = between(
            stages,
            ChunkVisibilityStage::MeshEligible,
            ChunkVisibilityStage::WorkerStart),
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
    ChunkVisibilityLifecycleKind kind,
    ChunkVisibilityOrigin origin) {
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
        .kind = kind,
        .origin = origin
    });
    advanceSequence(m_sequence);
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
        advanceSequence(m_sequence);
        return;
    }
    if (!record->meshTask) {
        record->meshTask = meshTask;
        advanceSequence(m_sequence);
    }
}

void ChunkVisibilityTracer::markDataReady(
    const ChunkVisibilityLifecycleKey& key,
    ChunkVisibilityOrigin origin) {
    if (!traces(key.coord) ||
        (origin != ChunkVisibilityOrigin::Persisted &&
         origin != ChunkVisibilityOrigin::Generated)) {
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        auto record = findRecord(key);
        if (record == m_records.end()) {
            ++m_unmatchedEvents;
            advanceSequence(m_sequence);
            return;
        }
        if (record->observed(ChunkVisibilityStage::DataReady)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }

    bool changed = false;
    if (record->origin == ChunkVisibilityOrigin::Unresolved) {
        record->origin = origin;
        changed = true;
    }
    auto& dataReady =
        record->stages[stageIndex(ChunkVisibilityStage::DataReady)];
    auto& dataReadyObserved =
        record->observedStages[stageIndex(ChunkVisibilityStage::DataReady)];
    if (!dataReadyObserved) {
        dataReadyObserved = true;
        changed = true;
    }
    if (!dataReady && timestamp) {
        dataReady = *timestamp;
        changed = true;
    }
    if (changed) {
        advanceSequence(m_sequence);
    }
}

void ChunkVisibilityTracer::observeMissingDesiredCardinalNeighborCount(
    const ChunkVisibilityLifecycleKey& key,
    uint8_t count) {
    if (!traces(key.coord)) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (!record->firstObservedMissingDesiredCardinalNeighborCount) {
        record->firstObservedMissingDesiredCardinalNeighborCount = count;
        advanceSequence(m_sequence);
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
            advanceSequence(m_sequence);
            return;
        }
        const bool needsObservation = std::any_of(
            stageValues.begin(), stageValues.end(),
            [&](ChunkVisibilityStage stage) {
                return stage != ChunkVisibilityStage::Count &&
                    stage != ChunkVisibilityStage::ResultAccepted &&
                    stage != ChunkVisibilityStage::FirstDraw &&
                    !record->observed(stage);
            });
        if (!needsObservation) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    bool changed = false;
    for (ChunkVisibilityStage stage : stageValues) {
        if (stage == ChunkVisibilityStage::Count ||
            stage == ChunkVisibilityStage::ResultAccepted ||
            stage == ChunkVisibilityStage::FirstDraw) {
            continue;
        }
        auto& stageTime = record->stages[stageIndex(stage)];
        auto& stageObserved = record->observedStages[stageIndex(stage)];
        if (!stageObserved) {
            stageObserved = true;
            if (timestamp) {
                stageTime = *timestamp;
            }
            changed = true;
        }
    }
    if (changed) {
        advanceSequence(m_sequence);
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
            advanceSequence(m_sequence);
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
        advanceSequence(m_sequence);
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
    if (outcome == ChunkVisibilityOutcome::AcceptedEmptyGeometry ||
        outcome == ChunkVisibilityOutcome::AcceptedNonemptyGeometry) {
        record->observedStages[
            stageIndex(ChunkVisibilityStage::ResultAccepted)] = true;
    }
    record->terminalTime = timestamp;
    record->outcome = outcome;
    advanceSequence(m_sequence);
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
            advanceSequence(m_sequence);
            return;
        }
        if (!canRecordDrawTransition(*record)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (!canRecordDrawTransition(*record)) {
        return;
    }
    if (timestamp) {
        record->stages[stageIndex(ChunkVisibilityStage::FirstDraw)] =
            *timestamp;
    }
    record->observedStages[stageIndex(ChunkVisibilityStage::FirstDraw)] = true;
    record->drawOutcome = ChunkVisibilityDrawOutcome::Drawn;
    record->drawTerminalTime = timestamp;
    advanceSequence(m_sequence);
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
            advanceSequence(m_sequence);
            return;
        }
        if (!canRecordDrawTransition(*record)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (canRecordDrawTransition(*record)) {
        record->drawOutcome = outcome;
        record->drawTerminalTime = timestamp;
        advanceSequence(m_sequence);
    }
}

ChunkVisibilityTraceMeasurement ChunkVisibilityTracer::measurement() const {
    if (!enabled()) {
        return {};
    }
    std::lock_guard lock(m_mutex);
    return {
        .sequence = m_sequence,
        .records = {m_records.begin(), m_records.end()},
        .stats = {
            .retainedRecords = m_records.size(),
            .droppedRecords = m_droppedRecords,
            .droppedUnfinishedRecords = m_droppedUnfinishedRecords,
            .unmatchedEvents = m_unmatchedEvents,
            .clockFailures = m_clockFailures
        }
    };
}

std::vector<ChunkVisibilityTraceRecord> ChunkVisibilityTracer::snapshot() const {
    if (!enabled()) {
        return {};
    }
    std::lock_guard lock(m_mutex);
    return {m_records.begin(), m_records.end()};
}

std::optional<ChunkVisibilityTraceRecord>
ChunkVisibilityTracer::latestRecord() const {
    if (!enabled()) {
        return std::nullopt;
    }
    std::lock_guard lock(m_mutex);
    if (m_records.empty()) {
        return std::nullopt;
    }
    return m_records.back();
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
        try {
            std::lock_guard lock(m_mutex);
            ++m_clockFailures;
            advanceSequence(m_sequence);
        } catch (...) {
        }
        return std::nullopt;
    }
}

} // namespace Rigel::Voxel
