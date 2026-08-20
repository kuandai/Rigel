#pragma once

#include <cstdint>

namespace Rigel::Voxel {

enum class ChunkLoadRequestResult : uint8_t {
    Missing,
    Queued,
    Deferred
};

} // namespace Rigel::Voxel
