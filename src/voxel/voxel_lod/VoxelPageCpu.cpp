#include "Rigel/Voxel/VoxelLod/VoxelPageCpu.h"

namespace Rigel::Voxel {

VoxelPageCpu buildVoxelPageCpu(const VoxelPageKey& key,
                               std::span<const VoxelId> l0,
                               int dim) {
    VoxelPageCpu out;
    if (dim <= 0) {
        return out;
    }
    const size_t expected = static_cast<size_t>(dim) *
        static_cast<size_t>(dim) *
        static_cast<size_t>(dim);
    if (l0.size() != expected) {
        return out;
    }

    out.key = key;
    out.dim = dim;
    out.l0.assign(l0.begin(), l0.end());
    out.mips = buildVoxelMipPyramid(out.l0, dim);
    return out;
}

} // namespace Rigel::Voxel

