#pragma once

#include "Rigel/Voxel/StreamingConfig.h"

#include <cstddef>

namespace Rigel::detail {

struct StreamingPolicy {
    Voxel::StreamingConfig streamer;
    size_t ioThreads = 1;
    size_t loadWorkerThreads = 1;
    size_t loadRegionDrainBudget = 16;
    size_t loadQueueLimit = 0;
    size_t loadMaxCachedRegions = 16;
    size_t loadMaxInFlightRegions = 16;
};

// Installed desktop policy. The overload taking a processor count is the
// deterministic seam used by tests; normal startup detects the host topology.
StreamingPolicy makeAutomaticStreamingPolicy();
StreamingPolicy makeAutomaticStreamingPolicy(
    unsigned logicalProcessorCount);

} // namespace Rigel::detail
