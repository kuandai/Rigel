#pragma once

#include <cstddef>

namespace Rigel::Voxel {

// Exact low-level scheduler inputs for tests and developer benchmarks.
// Installed application startup obtains this value from internal automatic
// policy; it is not parsed from player, world, or content configuration.
struct StreamingConfig {
    // Exact radii remain available to benchmark and test inputs. Normal
    // active worlds replace both from their effective View Distance policy.
    int viewDistanceChunks = 6;
    int unloadDistanceChunks = 8;
    size_t genQueueLimit = 0;
    size_t meshQueueLimit = 0;
    int updateBudgetPerFrame = 0;
    int applyBudgetPerFrame = 0;
    int workerThreads = 2;
    int loadApplyBudgetPerFrame = 8;
    size_t maxResidentChunks = 0;
};

} // namespace Rigel::Voxel
