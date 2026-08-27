#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Rigel::Voxel {

class ViewDistancePolicy final {
public:
    static std::shared_ptr<const ViewDistancePolicy> derive(
        int acceptedViewDistanceChunks,
        int persistenceRegionSpanChunks,
        uint64_t generation);

    int viewDistanceChunks() const { return m_viewDistanceChunks; }
    int desiredRadiusChunks() const { return m_desiredRadiusChunks; }
    int unloadRadiusChunks() const { return m_unloadRadiusChunks; }
    float renderDistanceWorldUnits() const {
        return m_renderDistanceWorldUnits;
    }
    float projectionFarPlaneWorldUnits() const {
        return m_projectionFarPlaneWorldUnits;
    }
    int preloadRadiusRegions() const { return m_preloadRadiusRegions; }
    size_t preloadRegionsPerRequest() const {
        return m_preloadRegionsPerRequest;
    }
    float shadowDistanceCeilingWorldUnits() const {
        return m_shadowDistanceCeilingWorldUnits;
    }
    uint64_t generation() const { return m_generation; }

private:
    ViewDistancePolicy(
        int viewDistanceChunks,
        int desiredRadiusChunks,
        int unloadRadiusChunks,
        float renderDistanceWorldUnits,
        float projectionFarPlaneWorldUnits,
        int preloadRadiusRegions,
        size_t preloadRegionsPerRequest,
        float shadowDistanceCeilingWorldUnits,
        uint64_t generation);

    int m_viewDistanceChunks = 0;
    int m_desiredRadiusChunks = 0;
    int m_unloadRadiusChunks = 0;
    float m_renderDistanceWorldUnits = 0.0f;
    float m_projectionFarPlaneWorldUnits = 0.0f;
    int m_preloadRadiusRegions = 0;
    size_t m_preloadRegionsPerRequest = 0;
    float m_shadowDistanceCeilingWorldUnits = 0.0f;
    uint64_t m_generation = 0;
};

} // namespace Rigel::Voxel
