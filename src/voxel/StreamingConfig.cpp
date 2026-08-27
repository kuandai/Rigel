#include "Rigel/Voxel/StreamingConfig.h"

#include "Rigel/Util/Yaml.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <algorithm>

namespace Rigel::Voxel {

void StreamingConfig::applyYaml(const char* sourceName, const std::string& yaml) {
    if (yaml.empty()) {
        return;
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(sourceName),
        ryml::to_csubstr(yaml)
    );
    const ryml::ConstNodeRef root = tree.rootref();
    if (!root.has_child("streaming")) {
        return;
    }

    const ryml::ConstNodeRef streamNode = root["streaming"];
    Util::warnUnknownKeys(
        streamNode,
        sourceName,
        "streaming",
        {
            "unload_distance_chunks", "gen_queue_limit", "mesh_queue_limit",
            "update_budget_per_frame", "apply_budget_per_frame",
            "worker_threads", "io_threads", "load_worker_threads",
            "load_apply_budget_per_frame", "load_region_drain_budget",
            "load_queue_limit", "load_max_cached_regions",
            "load_max_inflight_regions", "load_prefetch_radius",
            "load_prefetch_per_request", "max_resident_chunks"
        }
    );

    unloadDistanceChunks = Util::readIntWithMaximum(
        streamNode, "unload_distance_chunks", unloadDistanceChunks, 0,
        MaxUnloadDistanceChunks, sourceName, "streaming");

    int genLimit = Util::readIntWithMaximum(
        streamNode, "gen_queue_limit", static_cast<int>(genQueueLimit), 0,
        MaxQueueLimit, sourceName, "streaming");
    genQueueLimit = static_cast<size_t>(genLimit);

    int meshLimit = Util::readIntWithMaximum(
        streamNode, "mesh_queue_limit", static_cast<int>(meshQueueLimit), 0,
        MaxQueueLimit, sourceName, "streaming");
    meshQueueLimit = static_cast<size_t>(meshLimit);

    updateBudgetPerFrame = Util::readIntWithMaximum(
        streamNode, "update_budget_per_frame", updateBudgetPerFrame, 0,
        MaxBudgetPerFrame, sourceName, "streaming");
    applyBudgetPerFrame = Util::readIntWithMaximum(
        streamNode, "apply_budget_per_frame", applyBudgetPerFrame, 0,
        MaxBudgetPerFrame, sourceName, "streaming");
    workerThreads = Util::readIntWithMaximum(
        streamNode, "worker_threads", workerThreads, 0,
        MaxTotalWorkerThreads, sourceName, "streaming");
    ioThreads = Util::readIntWithMaximum(
        streamNode, "io_threads", ioThreads, 0,
        MaxTotalWorkerThreads, sourceName, "streaming");
    loadWorkerThreads = Util::readIntWithMaximum(
        streamNode, "load_worker_threads", loadWorkerThreads, 0,
        MaxTotalWorkerThreads, sourceName, "streaming");
    loadApplyBudgetPerFrame = Util::readIntWithMaximum(
        streamNode, "load_apply_budget_per_frame", loadApplyBudgetPerFrame, 0,
        MaxBudgetPerFrame, sourceName, "streaming");
    loadRegionDrainBudget = Util::readIntWithMaximum(
        streamNode, "load_region_drain_budget", loadRegionDrainBudget, 0,
        MaxBudgetPerFrame, sourceName, "streaming");
    loadQueueLimit = Util::readIntWithMaximum(
        streamNode, "load_queue_limit", loadQueueLimit, 0,
        MaxQueueLimit, sourceName, "streaming");
    loadMaxCachedRegions = Util::readIntWithMaximum(
        streamNode, "load_max_cached_regions", loadMaxCachedRegions, 0,
        MaxCachedRegions, sourceName, "streaming");
    loadMaxInFlightRegions = Util::readIntWithMaximum(
        streamNode, "load_max_inflight_regions", loadMaxInFlightRegions, 0,
        MaxInFlightRegions, sourceName, "streaming");
    loadPrefetchRadius = Util::readIntWithMaximum(
        streamNode, "load_prefetch_radius", loadPrefetchRadius, 0,
        MaxPrefetchRadius, sourceName, "streaming");
    loadPrefetchPerRequest = Util::readIntWithMaximum(
        streamNode, "load_prefetch_per_request", loadPrefetchPerRequest, 0,
        MaxPrefetchPerRequest, sourceName, "streaming");

    int resident = Util::readIntWithMaximum(
        streamNode, "max_resident_chunks", static_cast<int>(maxResidentChunks),
        0, MaxResidentChunks, sourceName, "streaming");
    maxResidentChunks = static_cast<size_t>(resident);

}

void StreamingConfig::validate(const char* sourceName) const {
    if (workerThreads + ioThreads + loadWorkerThreads > MaxTotalWorkerThreads) {
        Util::throwConfigurationConstraint(
            sourceName,
            "streaming.worker_threads",
            "combined worker_threads, io_threads, and load_worker_threads "
            "must not exceed " + std::to_string(MaxTotalWorkerThreads)
        );
    }
}

} // namespace Rigel::Voxel
