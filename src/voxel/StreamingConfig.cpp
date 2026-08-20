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
            "view_distance_chunks", "unload_distance_chunks", "gen_queue_limit",
            "mesh_queue_limit", "update_budget_per_frame", "apply_budget_per_frame",
            "worker_threads", "io_threads", "load_worker_threads",
            "load_apply_budget_per_frame", "load_region_drain_budget",
            "load_queue_limit", "load_max_cached_regions",
            "load_max_inflight_regions", "load_prefetch_radius",
            "load_prefetch_per_request", "max_resident_chunks"
        }
    );

    viewDistanceChunks = Util::readInt(
        streamNode, "view_distance_chunks", viewDistanceChunks);
    unloadDistanceChunks = Util::readInt(
        streamNode, "unload_distance_chunks", unloadDistanceChunks);

    int genLimit = Util::readInt(
        streamNode, "gen_queue_limit", static_cast<int>(genQueueLimit));
    genQueueLimit = static_cast<size_t>(genLimit < 0 ? 0 : genLimit);

    int meshLimit = Util::readInt(
        streamNode, "mesh_queue_limit", static_cast<int>(meshQueueLimit));
    meshQueueLimit = static_cast<size_t>(meshLimit < 0 ? 0 : meshLimit);

    updateBudgetPerFrame = Util::readInt(
        streamNode, "update_budget_per_frame", updateBudgetPerFrame);
    applyBudgetPerFrame = Util::readInt(
        streamNode, "apply_budget_per_frame", applyBudgetPerFrame);
    workerThreads = Util::readInt(streamNode, "worker_threads", workerThreads);
    ioThreads = Util::readInt(streamNode, "io_threads", ioThreads);
    loadWorkerThreads = Util::readInt(
        streamNode, "load_worker_threads", loadWorkerThreads);
    loadApplyBudgetPerFrame = Util::readInt(
        streamNode, "load_apply_budget_per_frame", loadApplyBudgetPerFrame);
    loadRegionDrainBudget = Util::readInt(
        streamNode, "load_region_drain_budget", loadRegionDrainBudget);
    loadQueueLimit = Util::readInt(
        streamNode, "load_queue_limit", loadQueueLimit);
    loadMaxCachedRegions = Util::readInt(
        streamNode, "load_max_cached_regions", loadMaxCachedRegions);
    loadMaxInFlightRegions = Util::readInt(
        streamNode, "load_max_inflight_regions", loadMaxInFlightRegions);
    loadPrefetchRadius = Util::readInt(
        streamNode, "load_prefetch_radius", loadPrefetchRadius);
    loadPrefetchPerRequest = Util::readInt(
        streamNode, "load_prefetch_per_request", loadPrefetchPerRequest);

    updateBudgetPerFrame = std::max(0, updateBudgetPerFrame);
    applyBudgetPerFrame = std::max(0, applyBudgetPerFrame);
    workerThreads = std::max(0, workerThreads);
    ioThreads = std::max(0, ioThreads);
    loadWorkerThreads = std::max(0, loadWorkerThreads);
    loadApplyBudgetPerFrame = std::max(0, loadApplyBudgetPerFrame);
    loadRegionDrainBudget = std::max(0, loadRegionDrainBudget);
    loadQueueLimit = std::max(0, loadQueueLimit);
    loadMaxCachedRegions = std::max(0, loadMaxCachedRegions);
    loadMaxInFlightRegions = std::max(0, loadMaxInFlightRegions);
    loadPrefetchRadius = std::max(0, loadPrefetchRadius);
    loadPrefetchPerRequest = std::max(0, loadPrefetchPerRequest);

    int resident = Util::readInt(
        streamNode, "max_resident_chunks", static_cast<int>(maxResidentChunks));
    maxResidentChunks = static_cast<size_t>(resident < 0 ? 0 : resident);
}

} // namespace Rigel::Voxel
