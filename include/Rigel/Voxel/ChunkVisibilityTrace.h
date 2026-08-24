#pragma once

#include "ChunkCoord.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace Rigel::Voxel {

enum class ChunkVisibilityStage : uint8_t {
    Desired,
    DataRequest,
    DataReady,
    NeighborReady,
    MeshEligible,
    SchedulerWait,
    PoolSubmit,
    WorkerStart,
    WorkerFinish,
    ResultAccepted,
    FirstDraw,
    Count
};

std::string_view chunkVisibilityStageName(ChunkVisibilityStage stage);

enum class ChunkVisibilityOutcome : uint8_t {
    Pending,
    VoxelEmpty,
    CachedEmptyGeometry,
    CachedNonemptyGeometry,
    AcceptedEmptyGeometry,
    AcceptedNonemptyGeometry,
    Stale,
    Failed,
    CameraLeft,
    TracerReplaced,
    Reset,
    GeneratorReplaced,
    StreamerDestroyed
};

std::string_view chunkVisibilityOutcomeName(ChunkVisibilityOutcome outcome);

enum class ChunkVisibilityLifecycleKind : uint8_t {
    CameraDemand,
    Remesh
};

std::string_view chunkVisibilityLifecycleKindName(
    ChunkVisibilityLifecycleKind kind);

enum class ChunkVisibilityOrigin : uint8_t {
    Unresolved,
    ResidentLeftCensored,
    Persisted,
    Generated,
    Remesh
};

std::string_view chunkVisibilityOriginName(ChunkVisibilityOrigin origin);

enum class ChunkVisibilityDrawOutcome : uint8_t {
    Drawn,
    CameraLeftBeforeDraw,
    MeshRemovedBeforeDraw,
    MeshReplacedBeforeDraw,
    TraceReplacedBeforeDraw
};

std::string_view chunkVisibilityDrawOutcomeName(
    ChunkVisibilityDrawOutcome outcome);

struct ChunkVisibilityLifecycleKey {
    ChunkCoord coord{};
    uint64_t lifecycleId = 0;

    bool operator==(const ChunkVisibilityLifecycleKey&) const = default;
};

struct ChunkVisibilityMeshTaskIdentity {
    uint64_t requestId = 0;
    uint64_t workEpoch = 0;
    uint64_t chunkInstanceId = 0;
    uint32_t revision = 0;

    bool operator==(const ChunkVisibilityMeshTaskIdentity&) const = default;
};

using ChunkVisibilityClock = std::chrono::steady_clock;
using ChunkVisibilityTimePoint = ChunkVisibilityClock::time_point;
using ChunkVisibilityDuration = ChunkVisibilityClock::duration;
using ChunkVisibilityStageTimes = std::array<
    std::optional<ChunkVisibilityTimePoint>,
    static_cast<size_t>(ChunkVisibilityStage::Count)>;
using ChunkVisibilityStageObservations = std::array<
    bool,
    static_cast<size_t>(ChunkVisibilityStage::Count)>;

struct ChunkVisibilityDurations {
    std::optional<ChunkVisibilityDuration> desiredToDataRequest;
    std::optional<ChunkVisibilityDuration> dataWait;
    std::optional<ChunkVisibilityDuration> dependencyWait;
    std::optional<ChunkVisibilityDuration> eligibilityWait;
    std::optional<ChunkVisibilityDuration> eligibleToWorkerStart;
    std::optional<ChunkVisibilityDuration> schedulerWait;
    std::optional<ChunkVisibilityDuration> poolWait;
    std::optional<ChunkVisibilityDuration> workerExecution;
    std::optional<ChunkVisibilityDuration> resultWait;
    std::optional<ChunkVisibilityDuration> desiredToAccepted;
    std::optional<ChunkVisibilityDuration> desiredToFirstDraw;
};

struct ChunkVisibilityTraceRecord {
    ChunkVisibilityLifecycleKey key{};
    ChunkVisibilityLifecycleKind kind =
        ChunkVisibilityLifecycleKind::CameraDemand;
    ChunkVisibilityOrigin origin = ChunkVisibilityOrigin::Unresolved;
    std::optional<ChunkVisibilityMeshTaskIdentity> meshTask;
    std::optional<uint8_t> firstObservedMissingDesiredCardinalNeighborCount;
    ChunkVisibilityStageTimes stages{};
    ChunkVisibilityStageObservations observedStages{};
    ChunkVisibilityOutcome outcome = ChunkVisibilityOutcome::Pending;
    std::optional<ChunkVisibilityTimePoint> terminalTime;
    std::optional<ChunkVisibilityDrawOutcome> drawOutcome;
    std::optional<ChunkVisibilityTimePoint> drawTerminalTime;

    std::optional<ChunkVisibilityTimePoint> stage(
        ChunkVisibilityStage value) const;
    bool observed(ChunkVisibilityStage value) const;
    ChunkVisibilityDurations durations() const;
};

struct ChunkVisibilityTraceStats {
    size_t retainedRecords = 0;
    uint64_t droppedRecords = 0;
    uint64_t droppedUnfinishedRecords = 0;
    uint64_t unmatchedEvents = 0;
    uint64_t clockFailures = 0;
};

struct ChunkVisibilityTraceMeasurement {
    uint64_t sequence = 0;
    std::vector<ChunkVisibilityTraceRecord> records;
    ChunkVisibilityTraceStats stats;
};

// Opt-in trace for repeated visibility lifecycles of one identified chunk.
// All retained records, including incomplete records, count toward capacity.
class ChunkVisibilityTracer {
public:
    struct Config {
        ChunkCoord coord{};
        size_t capacity = 0;
    };

    using Clock = std::function<ChunkVisibilityTimePoint()>;

    explicit ChunkVisibilityTracer(Config config, Clock clock = {});

    bool enabled() const { return m_config.capacity > 0; }
    bool traces(ChunkCoord coord) const {
        return enabled() && coord == m_config.coord;
    }
    size_t capacity() const { return m_config.capacity; }
    ChunkCoord coord() const { return m_config.coord; }

    std::optional<ChunkVisibilityLifecycleKey> begin(
        ChunkVisibilityLifecycleKind kind,
        ChunkVisibilityOrigin origin = ChunkVisibilityOrigin::Unresolved);
    void bindMeshTask(
        const ChunkVisibilityLifecycleKey& key,
        const ChunkVisibilityMeshTaskIdentity& meshTask);
    void markDataReady(
        const ChunkVisibilityLifecycleKey& key,
        ChunkVisibilityOrigin origin);
    void observeMissingDesiredCardinalNeighborCount(
        const ChunkVisibilityLifecycleKey& key,
        uint8_t count);
    void mark(const ChunkVisibilityLifecycleKey& key,
              ChunkVisibilityStage stage);
    void mark(const ChunkVisibilityLifecycleKey& key,
              std::initializer_list<ChunkVisibilityStage> stages);
    void complete(const ChunkVisibilityLifecycleKey& key,
                  ChunkVisibilityOutcome outcome);
    void observeDraw(const ChunkVisibilityLifecycleKey& key);
    void observeMeshUnavailable(
        const ChunkVisibilityLifecycleKey& key,
        ChunkVisibilityDrawOutcome outcome);

    ChunkVisibilityTraceMeasurement measurement() const;
    std::vector<ChunkVisibilityTraceRecord> snapshot() const;
    std::optional<ChunkVisibilityTraceRecord> latestRecord() const;

private:
    using RecordIterator = std::deque<ChunkVisibilityTraceRecord>::iterator;

    RecordIterator findRecord(const ChunkVisibilityLifecycleKey& key);
    std::optional<ChunkVisibilityTimePoint> now() const noexcept;

    Config m_config;
    Clock m_clock;
    mutable std::mutex m_clockMutex;
    mutable std::mutex m_mutex;
    std::deque<ChunkVisibilityTraceRecord> m_records;
    uint64_t m_droppedRecords = 0;
    uint64_t m_droppedUnfinishedRecords = 0;
    uint64_t m_unmatchedEvents = 0;
    mutable uint64_t m_clockFailures = 0;
    mutable uint64_t m_sequence = 0;
};

struct ChunkVisibilityTraceLink {
    ChunkVisibilityLifecycleKey key{};
    ChunkVisibilityLifecycleKind kind =
        ChunkVisibilityLifecycleKind::CameraDemand;
    std::shared_ptr<ChunkVisibilityTracer> tracer;
};

} // namespace Rigel::Voxel
