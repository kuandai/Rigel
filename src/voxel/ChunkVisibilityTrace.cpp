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
    return !record.drawOutcome &&
        (record.outcome == ChunkVisibilityOutcome::Pending ||
         hasPendingDraw(record));
}

bool canRecordFirstDraw(const ChunkVisibilityTraceRecord& record) {
    return !record.drawOutcome &&
        (record.outcome == ChunkVisibilityOutcome::Pending ||
         hasPendingDraw(record));
}

bool canMutateLifecycle(const ChunkVisibilityTraceRecord& record) {
    return record.outcome == ChunkVisibilityOutcome::Pending;
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
        case ChunkVisibilityStage::GenerationSchedulerPending:
            return "generation_scheduler_pending";
        case ChunkVisibilityStage::GenerationCapacityWait:
            return "generation_capacity_wait";
        case ChunkVisibilityStage::GenerationExecutorSubmit:
            return "generation_executor_submitted";
        case ChunkVisibilityStage::GenerationWorkerStart:
            return "generation_worker_start";
        case ChunkVisibilityStage::GenerationWorkerFinish:
            return "generation_worker_finish";
        case ChunkVisibilityStage::GenerationReady:
            return "generation_ready";
        case ChunkVisibilityStage::DataReady:
            return "data_ready";
        case ChunkVisibilityStage::NeighborReady:
            return "neighbor_ready";
        case ChunkVisibilityStage::MeshEligible:
            return "mesh_eligible";
        case ChunkVisibilityStage::SchedulerWait:
            return "mesh_scheduler_pending";
        case ChunkVisibilityStage::PoolSubmit:
            return "mesh_pool_submit";
        case ChunkVisibilityStage::WorkerStart:
            return "mesh_worker_start";
        case ChunkVisibilityStage::WorkerFinish:
            return "mesh_worker_finish";
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

std::string_view chunkVisibilityBlockerStateName(
    ChunkVisibilityBlockerState state) {
    switch (state) {
        case ChunkVisibilityBlockerState::LoadRegionSchedulerPending:
            return "load_region_scheduler_pending";
        case ChunkVisibilityBlockerState::LoadRegionPoolQueued:
            return "load_region_pool_queued";
        case ChunkVisibilityBlockerState::LoadRegionWorkerRunning:
            return "load_region_worker_running";
        case ChunkVisibilityBlockerState::LoadRegionResultPublished:
            return "load_region_result_published";
        case ChunkVisibilityBlockerState::LoadRegionRetryWaiting:
            return "load_region_retry_waiting";
        case ChunkVisibilityBlockerState::LoadRegionTerminalFailed:
            return "load_region_terminal_failed";
        case ChunkVisibilityBlockerState::LoadPayloadSchedulerPending:
            return "load_payload_scheduler_pending";
        case ChunkVisibilityBlockerState::LoadPayloadPoolQueued:
            return "load_payload_pool_queued";
        case ChunkVisibilityBlockerState::LoadPayloadWorkerRunning:
            return "load_payload_worker_running";
        case ChunkVisibilityBlockerState::LoadPayloadResultPublished:
            return "load_payload_result_published";
        case ChunkVisibilityBlockerState::LoadPayloadTerminalFailed:
            return "load_payload_terminal_failed";
        case ChunkVisibilityBlockerState::GenerationSchedulerPending:
            return "generation_scheduler_pending";
        case ChunkVisibilityBlockerState::GenerationCapacityWaiting:
            return "generation_capacity_waiting";
        case ChunkVisibilityBlockerState::GenerationExecutorQueued:
            return "generation_executor_queued";
        case ChunkVisibilityBlockerState::GenerationWorkerRunning:
            return "generation_worker_running";
        case ChunkVisibilityBlockerState::GenerationResultPublished:
            return "generation_result_published";
        case ChunkVisibilityBlockerState::GenerationTerminalFailed:
            return "generation_terminal_failed";
        case ChunkVisibilityBlockerState::ReadyResident:
            return "ready_resident";
        case ChunkVisibilityBlockerState::NoLongerRequired:
            return "no_longer_required";
        case ChunkVisibilityBlockerState::Unowned:
            return "unowned";
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
        .generationQueueWait = between(
            stages,
            ChunkVisibilityStage::GenerationSchedulerPending,
            observed(ChunkVisibilityStage::GenerationCapacityWait)
                ? ChunkVisibilityStage::GenerationCapacityWait
                : ChunkVisibilityStage::GenerationExecutorSubmit),
        .generationCapacityWait = between(
            stages,
            ChunkVisibilityStage::GenerationCapacityWait,
            ChunkVisibilityStage::GenerationExecutorSubmit),
        .generationPoolWait = between(
            stages,
            ChunkVisibilityStage::GenerationExecutorSubmit,
            ChunkVisibilityStage::GenerationWorkerStart),
        .generationExecution = between(
            stages,
            ChunkVisibilityStage::GenerationWorkerStart,
            ChunkVisibilityStage::GenerationWorkerFinish),
        .generationResultWait = between(
            stages,
            ChunkVisibilityStage::GenerationReady,
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
        if (m_records.front().outcome != ChunkVisibilityOutcome::Pending &&
            !canRecordDrawTransition(m_records.front())) {
            retainTerminalKey(m_records.front().key);
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
        if (isRetiredTerminalKey(key)) {
            return;
        }
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (canMutateLifecycle(*record) && !record->meshTask) {
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
            if (isRetiredTerminalKey(key)) {
                return;
            }
            ++m_unmatchedEvents;
            advanceSequence(m_sequence);
            return;
        }
        if (!canMutateLifecycle(*record) ||
            record->observed(ChunkVisibilityStage::DataReady)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        if (isRetiredTerminalKey(key)) {
            return;
        }
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }

    if (!canMutateLifecycle(*record)) {
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
        if (isRetiredTerminalKey(key)) {
            return;
        }
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (canMutateLifecycle(*record) &&
        !record->firstObservedMissingDesiredCardinalNeighborCount) {
        record->firstObservedMissingDesiredCardinalNeighborCount = count;
        advanceSequence(m_sequence);
    }
}

void ChunkVisibilityTracer::observeBlockingDesiredCardinalNeighbors(
    const ChunkVisibilityLifecycleKey& key,
    ChunkVisibilityBlockingNeighborSnapshot neighbors) {
    if (!traces(key.coord)) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        if (isRetiredTerminalKey(key)) {
            return;
        }
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (!canMutateLifecycle(*record)) {
        return;
    }
    neighbors.count = std::min<uint8_t>(
        neighbors.count, static_cast<uint8_t>(DirectionCount));
    bool changed = false;
    if (!record->firstObservedBlockingDesiredCardinalNeighbors) {
        record->firstObservedBlockingDesiredCardinalNeighbors = neighbors;
        changed = true;
    }
    if (record->blockingDesiredCardinalNeighbors != neighbors) {
        record->blockingDesiredCardinalNeighbors = neighbors;
        changed = true;
    }
    if (changed) {
        advanceSequence(m_sequence);
    }
}

std::optional<ChunkVisibilityBlockingNeighborSnapshot>
ChunkVisibilityTracer::blockingDesiredCardinalNeighbors(
    const ChunkVisibilityLifecycleKey& key) const {
    if (!traces(key.coord)) {
        return std::nullopt;
    }
    std::lock_guard lock(m_mutex);
    const auto record = findRecord(key);
    return record == m_records.end()
        ? std::nullopt
        : record->blockingDesiredCardinalNeighbors;
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
            if (isRetiredTerminalKey(key)) {
                return;
            }
            ++m_unmatchedEvents;
            advanceSequence(m_sequence);
            return;
        }
        if (!canMutateLifecycle(*record)) {
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
        if (isRetiredTerminalKey(key)) {
            return;
        }
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (!canMutateLifecycle(*record)) {
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
            if (isRetiredTerminalKey(key)) {
                return;
            }
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
        if (isRetiredTerminalKey(key)) {
            return;
        }
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
            if (isRetiredTerminalKey(key)) {
                return;
            }
            ++m_unmatchedEvents;
            advanceSequence(m_sequence);
            return;
        }
        if (!canRecordFirstDraw(*record)) {
            return;
        }
    }

    const auto timestamp = now();
    std::lock_guard lock(m_mutex);
    auto record = findRecord(key);
    if (record == m_records.end()) {
        if (isRetiredTerminalKey(key)) {
            return;
        }
        ++m_unmatchedEvents;
        advanceSequence(m_sequence);
        return;
    }
    if (!canRecordFirstDraw(*record)) {
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
            if (isRetiredTerminalKey(key)) {
                return;
            }
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
        if (isRetiredTerminalKey(key)) {
            return;
        }
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

ChunkVisibilityTracer::ConstRecordIterator ChunkVisibilityTracer::findRecord(
    const ChunkVisibilityLifecycleKey& key) const {
    return std::find_if(
        m_records.begin(),
        m_records.end(),
        [&](const ChunkVisibilityTraceRecord& record) {
            return record.key == key;
        });
}

bool ChunkVisibilityTracer::isRetiredTerminalKey(
    const ChunkVisibilityLifecycleKey& key) const {
    return std::find(
               m_retiredTerminalKeys.begin(),
               m_retiredTerminalKeys.end(),
               key) != m_retiredTerminalKeys.end();
}

void ChunkVisibilityTracer::retainTerminalKey(
    const ChunkVisibilityLifecycleKey& key) {
    while (m_retiredTerminalKeys.size() >= m_config.capacity) {
        m_retiredTerminalKeys.pop_front();
    }
    m_retiredTerminalKeys.push_back(key);
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
