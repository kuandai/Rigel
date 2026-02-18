#pragma once

#include "Rigel/Voxel/VoxelLod/VoxelSource.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Rigel::Voxel {

// A mip pyramid built from a page-sized voxel brick.
//
// Each cell packs:
// - uniform bit (31): true when all underlying voxels in the cell volume are identical
// - representative value (0..15): the uniform value if uniform, otherwise a cheap "dominant"
//   value selected from the 8 child representatives.
//
// NOTE: the "dominant" value is computed from child representatives and is not guaranteed
// to be the true mode across all underlying voxels. It is sufficient for early mips-based
// collapse decisions and coarse fallback materials.
struct VoxelMipLevel {
    int dim = 0;
    std::vector<uint32_t> cells;

    bool empty() const { return dim <= 0 || cells.empty(); }
    size_t cellCount() const { return cells.size(); }

    static constexpr uint32_t kUniformMask = 0x80000000u;
    static constexpr uint32_t kValueMask = 0x0000FFFFu;

    static bool isUniform(uint32_t packed) {
        return (packed & kUniformMask) != 0;
    }

    static VoxelId value(uint32_t packed) {
        return static_cast<VoxelId>(packed & kValueMask);
    }

    static uint32_t pack(bool uniform, VoxelId value) {
        return (uniform ? kUniformMask : 0u) | static_cast<uint32_t>(value);
    }
};

struct VoxelMipPyramid {
    int baseDim = 0;
    std::vector<VoxelMipLevel> levels; // levels[0] is L0, levels.back() is 1^3.

    bool empty() const { return levels.empty(); }
    size_t levelCount() const { return levels.size(); }
};

// Build a mip pyramid from L0 voxels (dim^3 values).
//
// Precondition: baseDim is a power of two and l0.size() == baseDim^3.
VoxelMipPyramid buildVoxelMipPyramid(std::span<const VoxelId> l0, int baseDim);

} // namespace Rigel::Voxel

