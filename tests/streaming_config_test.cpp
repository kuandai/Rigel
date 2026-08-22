#include "TestFramework.h"

#include "Rigel/Voxel/StreamingConfig.h"

#include <fstream>
#include <sstream>

using namespace Rigel::Voxel;

TEST_CASE(StreamingConfig_ShippedStreamingDistances) {
    const std::string path =
        std::string(RIGEL_TEST_SOURCE_DIRECTORY) +
        "/assets/config/world_generation.yaml";
    std::ifstream input(path);
    CHECK(input.good());

    std::ostringstream yaml;
    yaml << input.rdbuf();

    StreamingConfig config;
    config.applyYaml(path.c_str(), yaml.str());

    CHECK_EQ(config.viewDistanceChunks, 12);
    CHECK_EQ(config.unloadDistanceChunks, 12);
}

TEST_CASE(StreamingConfig_ApplyYaml) {
    StreamingConfig config;
    config.applyYaml("test", R"(
streaming:
  view_distance_chunks: 3
  unload_distance_chunks: 5
  gen_queue_limit: 4
  mesh_queue_limit: 6
  update_budget_per_frame: 12
  apply_budget_per_frame: 9
  worker_threads: 0
  io_threads: 2
  load_worker_threads: 3
  load_apply_budget_per_frame: 8
  load_region_drain_budget: 7
  load_queue_limit: 11
  load_max_cached_regions: 13
  load_max_inflight_regions: 15
  load_prefetch_radius: 2
  load_prefetch_per_request: 17
  max_resident_chunks: 100
)");

    CHECK_EQ(config.viewDistanceChunks, 3);
    CHECK_EQ(config.unloadDistanceChunks, 5);
    CHECK_EQ(config.genQueueLimit, static_cast<size_t>(4));
    CHECK_EQ(config.meshQueueLimit, static_cast<size_t>(6));
    CHECK_EQ(config.updateBudgetPerFrame, 12);
    CHECK_EQ(config.applyBudgetPerFrame, 9);
    CHECK_EQ(config.workerThreads, 0);
    CHECK_EQ(config.ioThreads, 2);
    CHECK_EQ(config.loadWorkerThreads, 3);
    CHECK_EQ(config.loadApplyBudgetPerFrame, 8);
    CHECK_EQ(config.loadRegionDrainBudget, 7);
    CHECK_EQ(config.loadQueueLimit, 11);
    CHECK_EQ(config.loadMaxCachedRegions, 13);
    CHECK_EQ(config.loadMaxInFlightRegions, 15);
    CHECK_EQ(config.loadPrefetchRadius, 2);
    CHECK_EQ(config.loadPrefetchPerRequest, 17);
    CHECK_EQ(config.maxResidentChunks, static_cast<size_t>(100));
}

TEST_CASE(StreamingConfig_LayeredMergeAndClamp) {
    StreamingConfig config;
    config.applyYaml("base", R"(
streaming:
  view_distance_chunks: 9
  worker_threads: 4
  load_queue_limit: 20
)");
    config.applyYaml("override", R"(
streaming:
  worker_threads: -1
  load_queue_limit: -1
  max_resident_chunks: -1
)");

    CHECK_EQ(config.viewDistanceChunks, 9);
    CHECK_EQ(config.workerThreads, 0);
    CHECK_EQ(config.loadQueueLimit, 0);
    CHECK_EQ(config.maxResidentChunks, static_cast<size_t>(0));
}
