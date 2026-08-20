#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Rigel::Voxel {

struct StreamingWorkCount {
    size_t pending = 0;
    size_t inFlight = 0;
    uint64_t started = 0;

    bool empty() const {
        return pending == 0 && inFlight == 0;
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
    uint32_t stableUpdates = 0;

    bool workEmpty() const {
        return generation.empty() && chunkLoad.empty() && mesh.empty();
    }
};

} // namespace Rigel::Voxel
