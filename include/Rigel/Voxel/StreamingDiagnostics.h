#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Rigel::Voxel {

struct StreamingWorkCount {
    size_t pending = 0;
    size_t inFlight = 0;
    uint64_t started = 0;
    size_t terminalErrors = 0;
    std::string lastError;
    uint64_t failureVersion = 0;

    bool empty() const {
        return pending == 0 && inFlight == 0 && terminalErrors == 0;
    }
};

// Cumulative region scheduler accounting for one immutable admission origin.
// Pool resubmissions are a subset of pool submissions, while retry admissions
// are new logical admissions rather than resubmissions of an existing owner.
struct RegionSchedulerOriginDiagnostics {
    uint64_t logicalAdmissions = 0;
    uint64_t retryAdmissions = 0;
    uint64_t logicalPreStartCancellations = 0;
    uint64_t poolSubmissions = 0;
    uint64_t poolResubmissions = 0;
    uint64_t successfulPoolYields = 0;
    uint64_t terminalPoolCancellations = 0;
    uint64_t poolWorkerStarts = 0;
    uint64_t inlineExecutions = 0;
    uint64_t resultsPublished = 0;
    uint64_t resultsDrained = 0;
    uint64_t missingProbes = 0;
    uint64_t admissionToWorkerStartNanoseconds = 0;
    uint64_t maxAdmissionToWorkerStartNanoseconds = 0;
    uint64_t workerExecutionNanoseconds = 0;
    uint64_t maxWorkerExecutionNanoseconds = 0;
};

struct RegionSchedulerDiagnosticSnapshot {
    RegionSchedulerOriginDiagnostics directOrigin;
    RegionSchedulerOriginDiagnostics speculativeOrigin;
    uint64_t demandPromotions = 0;
    uint64_t usefulPrefetchCacheHits = 0;
    uint64_t speculativeEvictionsBeforeDemand = 0;

    // Current ownership can change without changing admission origin.
    size_t demandOwnedQueued = 0;
    size_t speculativeOwnedQueued = 0;
    // Includes submitted inline/pool work until its result is drained.
    size_t demandOwnedDispatchedUndrained = 0;
    size_t speculativeOwnedDispatchedUndrained = 0;

    // Unstarted speculative pool submissions eligible for bounded yield.
    // Running and completed work remains in dispatched-undrained instead.
    size_t speculativePoolJobsPending = 0;
    size_t maxSpeculativePoolJobsPending = 0;
    uint64_t speculativePoolYieldCalls = 0;
    uint64_t speculativePoolYieldCandidateVisits = 0;
    size_t maxSpeculativePoolYieldCandidateVisits = 0;
};

struct ChunkLoadDiagnosticSnapshot {
    StreamingWorkCount work;
    RegionSchedulerDiagnosticSnapshot regionScheduler;
};

enum class StreamingLifecycleState : uint8_t {
    DiscoveringSpawn,
    AwaitingInitialStream,
    Streaming,
    Stabilizing,
    Quiescent
};

inline std::string_view streamingLifecycleName(StreamingLifecycleState state) {
    switch (state) {
        case StreamingLifecycleState::DiscoveringSpawn:
            return "discovering_spawn";
        case StreamingLifecycleState::AwaitingInitialStream:
            return "awaiting_initial_stream";
        case StreamingLifecycleState::Streaming:
            return "streaming";
        case StreamingLifecycleState::Stabilizing:
            return "stabilizing";
        case StreamingLifecycleState::Quiescent:
            return "quiescent";
    }
    return "unknown";
}

struct StreamingDiagnosticSnapshot {
    static constexpr uint32_t QuiescenceUpdateWindow = 3;

    StreamingLifecycleState state = StreamingLifecycleState::DiscoveringSpawn;
    // One scheduler-owned planning reconciliation can remain after generator
    // replacement even when the previously clipped desired set was empty.
    size_t plannerReconciliationPending = 0;
    StreamingWorkCount generation;
    // Exact current owners used to validate completion-drain and canonical
    // scheduler quiescence without exposing scheduler containers.
    size_t sourceResolutionPending = 0;
    size_t generationSchedulerPending = 0;
    size_t generationCompletionsPending = 0;
    StreamingWorkCount chunkLoad;
    RegionSchedulerDiagnosticSnapshot regionScheduler;
    StreamingWorkCount mesh;
    size_t meshCompletionsPending = 0;
    size_t meshWorkerCount = 0;
    // Maximum mesh jobs submitted but not yet observed by the completion
    // drain, including the bounded inline executor path.
    size_t meshSubmissionLimit = 0;
    StreamingWorkCount eviction;
    size_t retiredWorkPending = 0;
    uint32_t stableUpdates = 0;

    bool workEmpty() const {
        return plannerReconciliationPending == 0 && generation.empty() &&
            chunkLoad.empty() && mesh.empty() && eviction.empty();
    }
};

inline bool streamingFailureSignatureChanged(
    const StreamingDiagnosticSnapshot& previous,
    const StreamingDiagnosticSnapshot& current) {
    auto workFailureChanged = [](const StreamingWorkCount& before,
                                 const StreamingWorkCount& after) {
        return before.terminalErrors != after.terminalErrors ||
            before.lastError != after.lastError ||
            before.failureVersion != after.failureVersion;
    };

    return workFailureChanged(previous.generation, current.generation) ||
        workFailureChanged(previous.chunkLoad, current.chunkLoad) ||
        workFailureChanged(previous.mesh, current.mesh) ||
        workFailureChanged(previous.eviction, current.eviction) ||
        previous.eviction.pending != current.eviction.pending;
}

} // namespace Rigel::Voxel
