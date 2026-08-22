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

struct ChunkLoadCompletion {
    ChunkCoord coord;
    ChunkLoadOutcome outcome = ChunkLoadOutcome::Missing;
    std::string error;
};

} // namespace Rigel::Voxel
