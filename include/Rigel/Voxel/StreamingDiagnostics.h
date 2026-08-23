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
    StreamingWorkCount generation;
    StreamingWorkCount chunkLoad;
    StreamingWorkCount mesh;
    size_t meshWorkerCount = 0;
    // Maximum mesh jobs submitted but not yet observed by the completion
    // drain, including the bounded inline executor path.
    size_t meshSubmissionLimit = 0;
    StreamingWorkCount eviction;
    uint32_t stableUpdates = 0;

    bool workEmpty() const {
        return generation.empty() && chunkLoad.empty() && mesh.empty() &&
            eviction.empty();
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
