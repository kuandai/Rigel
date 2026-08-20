#pragma once

#include <cstddef>
#include <string>

namespace Rigel::Voxel {

struct StreamingConfig {
    int viewDistanceChunks = 6;
    int unloadDistanceChunks = 8;
    size_t genQueueLimit = 0;
    size_t meshQueueLimit = 0;
    int updateBudgetPerFrame = 0;
    int applyBudgetPerFrame = 0;
    int workerThreads = 2;
    int ioThreads = 1;
    int loadWorkerThreads = 2;
    int loadApplyBudgetPerFrame = 8;
    int loadRegionDrainBudget = 32;
    int loadQueueLimit = 0;
    int loadMaxCachedRegions = 8;
    int loadMaxInFlightRegions = 8;
    int loadPrefetchRadius = 1;
    int loadPrefetchPerRequest = 12;
    size_t maxResidentChunks = 0;

    void applyYaml(const char* sourceName, const std::string& yaml);
};

} // namespace Rigel::Voxel
