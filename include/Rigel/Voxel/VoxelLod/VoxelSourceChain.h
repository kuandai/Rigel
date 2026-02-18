#pragma once

#include "Rigel/Voxel/VoxelLod/VoxelSource.h"

#include <atomic>
#include <cstdint>
#include <span>

namespace Rigel::Voxel {

struct VoxelSourceChainTelemetry {
    uint64_t bricksSampled = 0;
    uint64_t voxelsSampled = 0;
    uint64_t loadedHits = 0;
    uint64_t persistenceHits = 0;
    uint64_t generatorHits = 0;
};

// Simple priority chain: loaded -> persistence -> generator.
//
// This chain is worker-safe as long as the individual sources are worker-safe.
class VoxelSourceChain {
public:
    void setLoaded(const IVoxelSource* source) { m_loaded = source; }
    void setPersistence(const IVoxelSource* source) { m_persistence = source; }
    void setGenerator(const IVoxelSource* source) { m_generator = source; }

    BrickSampleStatus sampleBrick(const BrickSampleDesc& desc,
                                  std::span<VoxelId> out,
                                  const std::atomic_bool* cancel = nullptr);

    const VoxelSourceChainTelemetry& telemetry() const { return m_telemetry; }
    void resetTelemetry() { m_telemetry = {}; }

private:
    const IVoxelSource* m_loaded = nullptr;
    const IVoxelSource* m_persistence = nullptr;
    const IVoxelSource* m_generator = nullptr;
    VoxelSourceChainTelemetry m_telemetry{};
};

} // namespace Rigel::Voxel

