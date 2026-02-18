#include "Rigel/Voxel/VoxelLod/VoxelSurfaceExtraction.h"

#include <algorithm>

namespace Rigel::Voxel {
namespace {

size_t gridIndex(int x, int y, int z, const glm::ivec3& dims) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dims.x)
        + static_cast<size_t>(z) * static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y);
}

bool isSolid(VoxelId id) {
    return id != kVoxelAir;
}

} // namespace

MacroVoxelGrid buildMacroGridFromPage(const VoxelPageCpu& page, int cellSizeVoxels) {
    MacroVoxelGrid out{};
    if (page.dim <= 0 || page.mips.empty()) {
        return out;
    }

    const int clampedCell = std::max(1, cellSizeVoxels);
    if ((page.dim % clampedCell) != 0) {
        return out;
    }

    int mip = 0;
    int s = clampedCell;
    while (s > 1) {
        if ((s & 1) != 0) {
            return out;
        }
        s >>= 1;
        ++mip;
    }

    if (mip < 0 || mip >= static_cast<int>(page.mips.levels.size())) {
        return out;
    }

    const VoxelMipLevel& level = page.mips.levels[mip];
    if (level.empty()) {
        return out;
    }

    out.dims = glm::ivec3(level.dim);
    out.cellSizeVoxels = clampedCell;
    out.cells.resize(level.cells.size(), kVoxelAir);
    for (size_t i = 0; i < level.cells.size(); ++i) {
        out.cells[i] = VoxelMipLevel::value(level.cells[i]);
    }
    return out;
}

void extractSurfaceQuads(const MacroVoxelGrid& grid,
                         VoxelBoundaryPolicy boundaryPolicy,
                         std::vector<SurfaceQuad>& out) {
    out.clear();
    if (grid.empty()) {
        return;
    }

    const glm::ivec3 dims = grid.dims;
    auto sample = [&](int x, int y, int z) -> VoxelId {
        if (x < 0 || y < 0 || z < 0 || x >= dims.x || y >= dims.y || z >= dims.z) {
            return boundaryPolicy == VoxelBoundaryPolicy::OutsideEmpty ? kVoxelAir : static_cast<VoxelId>(1);
        }
        return grid.cells[gridIndex(x, y, z, dims)];
    };

    // Emit one quad per macro cell face (greedy merge is a later stage).
    for (int z = 0; z < dims.z; ++z) {
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                const VoxelId id = sample(x, y, z);
                if (!isSolid(id)) {
                    continue;
                }

                // +X
                if (!isSolid(sample(x + 1, y, z))) {
                    out.push_back(SurfaceQuad{
                        .normal = Direction::PosX,
                        .cellMin = glm::ivec3(x, y, z),
                        .span = glm::ivec2(1, 1),
                        .material = id
                    });
                }
                // -X
                if (!isSolid(sample(x - 1, y, z))) {
                    out.push_back(SurfaceQuad{
                        .normal = Direction::NegX,
                        .cellMin = glm::ivec3(x, y, z),
                        .span = glm::ivec2(1, 1),
                        .material = id
                    });
                }
                // +Y
                if (!isSolid(sample(x, y + 1, z))) {
                    out.push_back(SurfaceQuad{
                        .normal = Direction::PosY,
                        .cellMin = glm::ivec3(x, y, z),
                        .span = glm::ivec2(1, 1),
                        .material = id
                    });
                }
                // -Y
                if (!isSolid(sample(x, y - 1, z))) {
                    out.push_back(SurfaceQuad{
                        .normal = Direction::NegY,
                        .cellMin = glm::ivec3(x, y, z),
                        .span = glm::ivec2(1, 1),
                        .material = id
                    });
                }
                // +Z
                if (!isSolid(sample(x, y, z + 1))) {
                    out.push_back(SurfaceQuad{
                        .normal = Direction::PosZ,
                        .cellMin = glm::ivec3(x, y, z),
                        .span = glm::ivec2(1, 1),
                        .material = id
                    });
                }
                // -Z
                if (!isSolid(sample(x, y, z - 1))) {
                    out.push_back(SurfaceQuad{
                        .normal = Direction::NegZ,
                        .cellMin = glm::ivec3(x, y, z),
                        .span = glm::ivec2(1, 1),
                        .material = id
                    });
                }
            }
        }
    }
}

} // namespace Rigel::Voxel

