#pragma once

#include "ChunkCoord.h"

#include <cstdint>
#include <string>

namespace Rigel::Voxel {

enum class ChunkLoadRequestResult : uint8_t {
    Missing,
    Queued,
    Deferred
};

enum class ChunkLoadOutcome : uint8_t {
    Loaded,
    Missing,
    Failed
};

enum class ChunkLoadExecutionState : uint8_t {
    Pending,
    Running,
    FailedRetrying
};

using ChunkLoadRequestId = uint64_t;

struct ChunkLoadRequest {
    ChunkCoord coord;
    ChunkLoadRequestId requestId = 0;
};

struct ChunkLoadCompletion {
    ChunkCoord coord;
    ChunkLoadRequestId requestId = 0;
    ChunkLoadOutcome outcome = ChunkLoadOutcome::Missing;
    std::string error;
};

} // namespace Rigel::Voxel
