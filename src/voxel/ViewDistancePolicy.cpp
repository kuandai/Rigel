#include "Rigel/Voxel/ViewDistancePolicy.h"

#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/Voxel/Chunk.h"

#include <algorithm>
#include <stdexcept>

namespace Rigel::Voxel {
namespace {

constexpr int kUnloadHysteresisChunks = 1;
constexpr int kMaximumPreloadRadiusRegions = 2;
constexpr size_t kPreloadRegionsPerRequest = 12;
constexpr float kMinimumProjectionFarPlaneWorldUnits = 500.0f;

} // namespace

std::shared_ptr<const ViewDistancePolicy> ViewDistancePolicy::derive(
    int acceptedViewDistanceChunks,
    int persistenceRegionSpanChunks,
    uint64_t generation) {
    if (acceptedViewDistanceChunks <
            Preferences::kMinimumViewDistanceChunks ||
        acceptedViewDistanceChunks >
            Preferences::kMaximumViewDistanceChunks) {
        throw std::invalid_argument(
            "View Distance policy requires an accepted 2 through 16 chunk radius");
    }
    if (persistenceRegionSpanChunks < 1) {
        throw std::invalid_argument(
            "View Distance policy requires a positive persistence region span");
    }
    if (generation == 0) {
        throw std::invalid_argument(
            "View Distance policy generation must be nonzero");
    }

    const int unloadRadiusChunks =
        acceptedViewDistanceChunks + kUnloadHysteresisChunks;
    const float renderDistanceWorldUnits = static_cast<float>(
        (acceptedViewDistanceChunks + 1) * Chunk::SIZE);
    const float projectionFarPlaneWorldUnits = std::max(
        kMinimumProjectionFarPlaneWorldUnits,
        renderDistanceWorldUnits + static_cast<float>(Chunk::SIZE));
    const int preloadRadiusRegions = std::clamp(
        acceptedViewDistanceChunks / persistenceRegionSpanChunks,
        1,
        kMaximumPreloadRadiusRegions);

    return std::shared_ptr<const ViewDistancePolicy>(
        new ViewDistancePolicy(
            acceptedViewDistanceChunks,
            acceptedViewDistanceChunks,
            unloadRadiusChunks,
            renderDistanceWorldUnits,
            projectionFarPlaneWorldUnits,
            preloadRadiusRegions,
            kPreloadRegionsPerRequest,
            renderDistanceWorldUnits,
            generation));
}

ViewDistancePolicy::ViewDistancePolicy(
    int viewDistanceChunks,
    int desiredRadiusChunks,
    int unloadRadiusChunks,
    float renderDistanceWorldUnits,
    float projectionFarPlaneWorldUnits,
    int preloadRadiusRegions,
    size_t preloadRegionsPerRequest,
    float shadowDistanceCeilingWorldUnits,
    uint64_t generation)
    : m_viewDistanceChunks(viewDistanceChunks)
    , m_desiredRadiusChunks(desiredRadiusChunks)
    , m_unloadRadiusChunks(unloadRadiusChunks)
    , m_renderDistanceWorldUnits(renderDistanceWorldUnits)
    , m_projectionFarPlaneWorldUnits(projectionFarPlaneWorldUnits)
    , m_preloadRadiusRegions(preloadRadiusRegions)
    , m_preloadRegionsPerRequest(preloadRegionsPerRequest)
    , m_shadowDistanceCeilingWorldUnits(shadowDistanceCeilingWorldUnits)
    , m_generation(generation) {
}

} // namespace Rigel::Voxel
