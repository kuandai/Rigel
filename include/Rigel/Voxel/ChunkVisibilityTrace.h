#pragma once

#include "ChunkCoord.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
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
    AcceptedEmptyGeometry,
    AcceptedNonemptyGeometry,
    Stale,
    Failed
};

std::string_view chunkVisibilityOutcomeName(ChunkVisibilityOutcome outcome);

struct ChunkVisibilityTraceIdentity {
    ChunkCoord coord{};
    uint64_t requestId = 0;
    uint64_t workEpoch = 0;
    uint64_t chunkInstanceId = 0;
    uint32_t revision = 0;

    bool operator==(const ChunkVisibilityTraceIdentity&) const = default;
};

using ChunkVisibilityClock = std::chrono::steady_clock;
using ChunkVisibilityTimePoint = ChunkVisibilityClock::time_point;
using ChunkVisibilityDuration = ChunkVisibilityClock::duration;
using ChunkVisibilityStageTimes = std::array<
    std::optional<ChunkVisibilityTimePoint>,
    static_cast<size_t>(ChunkVisibilityStage::Count)>;

struct ChunkVisibilityDurations {
    std::optional<ChunkVisibilityDuration> desiredToDataRequest;
    std::optional<ChunkVisibilityDuration> dataWait;
    std::optional<ChunkVisibilityDuration> dependencyWait;
    std::optional<ChunkVisibilityDuration> eligibilityWait;
    std::optional<ChunkVisibilityDuration> schedulerWait;
    std::optional<ChunkVisibilityDuration> poolWait;
    std::optional<ChunkVisibilityDuration> workerExecution;
    std::optional<ChunkVisibilityDuration> resultWait;
    std::optional<ChunkVisibilityDuration> desiredToAccepted;
    std::optional<ChunkVisibilityDuration> desiredToFirstDraw;
};

struct ChunkVisibilityTraceRecord {
    ChunkVisibilityTraceIdentity identity{};
    ChunkVisibilityStageTimes stages{};
    ChunkVisibilityOutcome outcome = ChunkVisibilityOutcome::Pending;
    std::optional<ChunkVisibilityTimePoint> terminalTime;

    std::optional<ChunkVisibilityTimePoint> stage(
        ChunkVisibilityStage value) const;
    ChunkVisibilityDurations durations() const;
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

    // capture() is used for stages collected before a mesh identity is known.
    // A disabled tracer never invokes its clock.
    std::optional<ChunkVisibilityTimePoint> capture() const;

    void begin(const ChunkVisibilityTraceIdentity& identity,
               ChunkVisibilityStageTimes initialStages = {});
    void mark(const ChunkVisibilityTraceIdentity& identity,
              ChunkVisibilityStage stage);
    void complete(const ChunkVisibilityTraceIdentity& identity,
                  ChunkVisibilityOutcome outcome);
    void observeDraw(const ChunkVisibilityTraceIdentity& identity);

    std::vector<ChunkVisibilityTraceRecord> snapshot() const;

private:
    using RecordIterator = std::deque<ChunkVisibilityTraceRecord>::iterator;

    RecordIterator findRecord(const ChunkVisibilityTraceIdentity& identity);
    ChunkVisibilityTimePoint now() const;

    Config m_config;
    Clock m_clock;
    mutable std::mutex m_mutex;
    std::deque<ChunkVisibilityTraceRecord> m_records;
};

struct ChunkVisibilityTraceLink {
    ChunkVisibilityTraceIdentity identity{};
    std::weak_ptr<ChunkVisibilityTracer> tracer;
};

} // namespace Rigel::Voxel
