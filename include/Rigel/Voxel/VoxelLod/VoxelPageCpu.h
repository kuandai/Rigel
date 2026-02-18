#pragma once

#include "Rigel/Voxel/VoxelLod/VoxelMipPyramid.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Rigel::Voxel {

struct VoxelPageKey {
    int level = 0;
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const VoxelPageKey&) const = default;
};

struct VoxelPageKeyHash {
    size_t operator()(const VoxelPageKey& key) const noexcept {
        // Simple integer hash combining 4 signed coordinates.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(static_cast<uint32_t>(key.level));
        mix(static_cast<uint32_t>(key.x));
        mix(static_cast<uint32_t>(key.y));
        mix(static_cast<uint32_t>(key.z));
        return static_cast<size_t>(h);
    }
};

// CPU representation for a voxel page (page-sized brick + mip pyramid).
struct VoxelPageCpu {
    VoxelPageKey key{};
    int dim = 0; // L0 dimensions in samples (typically config.pageSizeVoxels).
    std::vector<VoxelId> l0; // dim^3 sampled voxel IDs at this page's scale.
    VoxelMipPyramid mips;

    size_t l0VoxelCount() const { return l0.size(); }

    size_t cpuBytes() const {
        size_t bytes = 0;
        bytes += l0.size() * sizeof(VoxelId);
        for (const VoxelMipLevel& level : mips.levels) {
            bytes += level.cells.size() * sizeof(uint32_t);
        }
        return bytes;
    }
};

// Construct a CPU page from an L0 brick.
//
// Returns an empty page if inputs are invalid.
VoxelPageCpu buildVoxelPageCpu(const VoxelPageKey& key,
                               std::span<const VoxelId> l0,
                               int dim);

} // namespace Rigel::Voxel
