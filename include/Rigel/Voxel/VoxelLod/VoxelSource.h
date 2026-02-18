#pragma once

#include "Rigel/Voxel/Block.h"

#include <glm/vec3.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Rigel::Voxel {

// Voxel payload used by the voxel SVO system.
//
// This is intentionally minimal: it maps directly to BlockID.type so the far
// system can remain format-agnostic and do type/material lookups via the
// BlockRegistry when needed.
using VoxelId = uint16_t;

inline constexpr VoxelId kVoxelAir = 0;

inline constexpr VoxelId toVoxelId(BlockID id) {
    return id.type;
}

enum class BrickSampleStatus : uint8_t {
    Hit,
    Miss,
    Cancelled
};

struct BrickSampleDesc {
    glm::ivec3 worldMinVoxel{0};
    glm::ivec3 brickDimsVoxels{0};
    int stepVoxels = 1;

    bool isValid() const {
        if (stepVoxels <= 0) {
            return false;
        }
        if (brickDimsVoxels.x <= 0 || brickDimsVoxels.y <= 0 || brickDimsVoxels.z <= 0) {
            return false;
        }
        return (brickDimsVoxels.x % stepVoxels) == 0 &&
            (brickDimsVoxels.y % stepVoxels) == 0 &&
            (brickDimsVoxels.z % stepVoxels) == 0;
    }

    glm::ivec3 outDims() const {
        if (!isValid()) {
            return glm::ivec3(0);
        }
        return brickDimsVoxels / stepVoxels;
    }

    size_t outVoxelCount() const {
        const glm::ivec3 dims = outDims();
        if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) {
            return 0;
        }
        return static_cast<size_t>(dims.x) *
            static_cast<size_t>(dims.y) *
            static_cast<size_t>(dims.z);
    }
};

class IVoxelSource {
public:
    virtual ~IVoxelSource() = default;

    // Sample a brick of voxels at world-space voxel coordinates.
    //
    // Sampling policy for step > 1:
    // - Output index (x,y,z) samples the voxel at:
    //   worldMinVoxel + ivec3(x * step, y * step, z * step)
    //
    // Precondition: out.size() == desc.outVoxelCount().
    virtual BrickSampleStatus sampleBrick(const BrickSampleDesc& desc,
                                          std::span<VoxelId> out,
                                          const std::atomic_bool* cancel = nullptr) const = 0;
};

} // namespace Rigel::Voxel

