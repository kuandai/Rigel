#include "TestFramework.h"

#include "Rigel/Voxel/StreamingConfig.h"

#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

using namespace Rigel::Voxel;

namespace {

std::string exceptionMessage(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    throw Rigel::Test::TestFailure("Expected invalid configuration");
}

} // namespace

TEST_CASE(StreamingConfig_ShippedPolicyOmitsOnlyPlayerViewDistance) {
    const std::string path =
        std::string(RIGEL_TEST_SOURCE_DIRECTORY) +
        "/assets/config/streaming.yaml";
    std::ifstream input(path);
    CHECK(input.good());

    std::ostringstream yaml;
    yaml << input.rdbuf();

    StreamingConfig config;
    config.applyYaml(path.c_str(), yaml.str());

    const std::string document = yaml.str();
    CHECK_EQ(document.find("view_distance_chunks"), std::string::npos);
    CHECK_NE(document.find("unload_distance_chunks"), std::string::npos);
    CHECK_NE(document.find("load_prefetch_radius"), std::string::npos);
    CHECK_EQ(config.viewDistanceChunks, 6);
    CHECK_EQ(config.unloadDistanceChunks, 13);
    CHECK_EQ(config.loadPrefetchRadius, 1);
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

    CHECK_EQ(config.viewDistanceChunks, 6);
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
    config.validate("merged test configuration");

    CHECK_EQ(config.viewDistanceChunks, 6);
    CHECK_EQ(config.unloadDistanceChunks, 8);
    CHECK_EQ(config.workerThreads, 0);
    CHECK_EQ(config.loadQueueLimit, 0);
    CHECK_EQ(config.maxResidentChunks, static_cast<size_t>(0));
}

TEST_CASE(StreamingConfig_AcceptsOperationalMaxima) {
    StreamingConfig config;
    config.applyYaml("limits.yaml", R"(
streaming:
  view_distance_chunks: 16
  unload_distance_chunks: 24
  gen_queue_limit: 32768
  mesh_queue_limit: 32768
  update_budget_per_frame: 32768
  apply_budget_per_frame: 32768
  worker_threads: 60
  io_threads: 2
  load_worker_threads: 2
  load_apply_budget_per_frame: 32768
  load_region_drain_budget: 32768
  load_queue_limit: 32768
  load_max_cached_regions: 256
  load_max_inflight_regions: 64
  load_prefetch_radius: 4
  load_prefetch_per_request: 728
  max_resident_chunks: 65536
)");
    config.validate("merged test configuration");

    CHECK_EQ(config.viewDistanceChunks, 6);
    CHECK_EQ(config.unloadDistanceChunks, 24);
    CHECK_EQ(config.genQueueLimit,
             static_cast<size_t>(StreamingConfig::MaxQueueLimit));
    CHECK_EQ(config.meshQueueLimit,
             static_cast<size_t>(StreamingConfig::MaxQueueLimit));
    CHECK_EQ(config.workerThreads + config.ioThreads + config.loadWorkerThreads,
             StreamingConfig::MaxTotalWorkerThreads);
    CHECK_EQ(config.loadPrefetchPerRequest,
             StreamingConfig::MaxPrefetchPerRequest);
    CHECK_EQ(config.loadPrefetchRadius,
             StreamingConfig::MaxPrefetchRadius);
}

TEST_CASE(StreamingConfig_RejectsValuesAboveOperationalMaxima) {
    const std::string workerError = exceptionMessage([] {
        StreamingConfig config;
        config.applyYaml(
            "limits.yaml",
            "streaming:\n  worker_threads: 65\n"
        );
    });
    CHECK_EQ(
        workerError,
        "Invalid configuration value 'streaming.worker_threads' in "
        "'limits.yaml': expected integer no greater than 64, got '65'"
    );

    const std::string queueError = exceptionMessage([] {
        StreamingConfig config;
        config.applyYaml(
            "limits.yaml",
            "streaming:\n  gen_queue_limit: 2147483647\n"
        );
    });
    CHECK_EQ(
        queueError,
        "Invalid configuration value 'streaming.gen_queue_limit' in "
        "'limits.yaml': expected integer no greater than 32768, got "
        "'2147483647'"
    );

    const std::string unsignedError = exceptionMessage([] {
        StreamingConfig config;
        config.applyYaml(
            "limits.yaml",
            "streaming:\n  load_queue_limit: 4294967295\n"
        );
    });
    CHECK_EQ(
        unsignedError,
        "Invalid configuration value 'streaming.load_queue_limit' in "
        "'limits.yaml': expected integer no greater than 32768, got "
        "'4294967295'"
    );

    const std::string prefetchError = exceptionMessage([] {
        StreamingConfig config;
        config.applyYaml(
            "limits.yaml",
            "streaming:\n  load_prefetch_per_request: 729\n"
        );
    });
    CHECK_EQ(
        prefetchError,
        "Invalid configuration value 'streaming.load_prefetch_per_request' "
        "in 'limits.yaml': expected integer no greater than 728, got '729'"
    );
}

TEST_CASE(StreamingConfig_RejectsCrossFieldViolations) {
    const std::string workerError = exceptionMessage([] {
        StreamingConfig config;
        config.applyYaml(
            "constraints.yaml",
            "streaming:\n"
            "  worker_threads: 61\n"
            "  io_threads: 2\n"
            "  load_worker_threads: 2\n"
        );
        config.validate("merged test configuration");
    });
    CHECK_EQ(
        workerError,
        "Invalid configuration value 'streaming.worker_threads' in "
        "'merged test configuration': combined worker_threads, io_threads, and "
        "load_worker_threads must not exceed 64"
    );
}

TEST_CASE(StreamingConfig_ValidatesWorkerTotalAfterLayeredMerge) {
    StreamingConfig config;
    config.applyYaml(
        "base.yaml",
        "streaming:\n  worker_threads: 64\n"
    );
    config.applyYaml(
        "override.yaml",
        "streaming:\n  io_threads: 0\n  load_worker_threads: 0\n"
    );
    config.validate("merged test configuration");

    CHECK_EQ(config.workerThreads, 64);
    CHECK_EQ(config.ioThreads, 0);
    CHECK_EQ(config.loadWorkerThreads, 0);
}
