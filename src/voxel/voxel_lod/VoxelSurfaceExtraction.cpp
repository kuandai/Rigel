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

void extractSurfaceQuadsGreedy(const MacroVoxelGrid& grid,
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

    auto greedy2d = [&](int width,
                        int height,
                        std::vector<VoxelId>& mask,
                        auto emit) {
        for (int j = 0; j < height; ++j) {
            for (int i = 0; i < width; ++i) {
                const size_t idx0 = static_cast<size_t>(i) + static_cast<size_t>(j) * static_cast<size_t>(width);
                const VoxelId material = mask[idx0];
                if (!isSolid(material)) {
                    continue;
                }

                int runW = 1;
                while (i + runW < width) {
                    const size_t idx = static_cast<size_t>(i + runW)
                        + static_cast<size_t>(j) * static_cast<size_t>(width);
                    if (mask[idx] != material) {
                        break;
                    }
                    ++runW;
                }

                int runH = 1;
                bool done = false;
                while (j + runH < height && !done) {
                    for (int k = 0; k < runW; ++k) {
                        const size_t idx = static_cast<size_t>(i + k)
                            + static_cast<size_t>(j + runH) * static_cast<size_t>(width);
                        if (mask[idx] != material) {
                            done = true;
                            break;
                        }
                    }
                    if (!done) {
                        ++runH;
                    }
                }

                emit(i, j, runW, runH, material);

                for (int y = 0; y < runH; ++y) {
                    for (int x = 0; x < runW; ++x) {
                        const size_t idx = static_cast<size_t>(i + x)
                            + static_cast<size_t>(j + y) * static_cast<size_t>(width);
                        mask[idx] = kVoxelAir;
                    }
                }
            }
        }
    };

    // +/-X faces: sweep over x, greedy on (z,y).
    for (int x = 0; x < dims.x; ++x) {
        std::vector<VoxelId> posMask(static_cast<size_t>(dims.z) * dims.y, kVoxelAir);
        std::vector<VoxelId> negMask(static_cast<size_t>(dims.z) * dims.y, kVoxelAir);
        for (int y = 0; y < dims.y; ++y) {
            for (int z = 0; z < dims.z; ++z) {
                const VoxelId id = sample(x, y, z);
                if (!isSolid(id)) {
                    continue;
                }
                const size_t idx = static_cast<size_t>(z) + static_cast<size_t>(y) * static_cast<size_t>(dims.z);
                if (!isSolid(sample(x + 1, y, z))) {
                    posMask[idx] = id;
                }
                if (!isSolid(sample(x - 1, y, z))) {
                    negMask[idx] = id;
                }
            }
        }

        greedy2d(dims.z, dims.y, posMask, [&](int z0, int y0, int zSpan, int ySpan, VoxelId mat) {
            out.push_back(SurfaceQuad{
                .normal = Direction::PosX,
                .cellMin = glm::ivec3(x, y0, z0),
                .span = glm::ivec2(zSpan, ySpan),
                .material = mat
            });
        });
        greedy2d(dims.z, dims.y, negMask, [&](int z0, int y0, int zSpan, int ySpan, VoxelId mat) {
            out.push_back(SurfaceQuad{
                .normal = Direction::NegX,
                .cellMin = glm::ivec3(x, y0, z0),
                .span = glm::ivec2(zSpan, ySpan),
                .material = mat
            });
        });
    }

    // +/-Y faces: sweep over y, greedy on (x,z).
    for (int y = 0; y < dims.y; ++y) {
        std::vector<VoxelId> posMask(static_cast<size_t>(dims.x) * dims.z, kVoxelAir);
        std::vector<VoxelId> negMask(static_cast<size_t>(dims.x) * dims.z, kVoxelAir);
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                const VoxelId id = sample(x, y, z);
                if (!isSolid(id)) {
                    continue;
                }
                const size_t idx = static_cast<size_t>(x) + static_cast<size_t>(z) * static_cast<size_t>(dims.x);
                if (!isSolid(sample(x, y + 1, z))) {
                    posMask[idx] = id;
                }
                if (!isSolid(sample(x, y - 1, z))) {
                    negMask[idx] = id;
                }
            }
        }

        greedy2d(dims.x, dims.z, posMask, [&](int x0, int z0, int xSpan, int zSpan, VoxelId mat) {
            out.push_back(SurfaceQuad{
                .normal = Direction::PosY,
                .cellMin = glm::ivec3(x0, y, z0),
                .span = glm::ivec2(xSpan, zSpan),
                .material = mat
            });
        });
        greedy2d(dims.x, dims.z, negMask, [&](int x0, int z0, int xSpan, int zSpan, VoxelId mat) {
            out.push_back(SurfaceQuad{
                .normal = Direction::NegY,
                .cellMin = glm::ivec3(x0, y, z0),
                .span = glm::ivec2(xSpan, zSpan),
                .material = mat
            });
        });
    }

    // +/-Z faces: sweep over z, greedy on (x,y).
    for (int z = 0; z < dims.z; ++z) {
        std::vector<VoxelId> posMask(static_cast<size_t>(dims.x) * dims.y, kVoxelAir);
        std::vector<VoxelId> negMask(static_cast<size_t>(dims.x) * dims.y, kVoxelAir);
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                const VoxelId id = sample(x, y, z);
                if (!isSolid(id)) {
                    continue;
                }
                const size_t idx = static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(dims.x);
                if (!isSolid(sample(x, y, z + 1))) {
                    posMask[idx] = id;
                }
                if (!isSolid(sample(x, y, z - 1))) {
                    negMask[idx] = id;
                }
            }
        }

        greedy2d(dims.x, dims.y, posMask, [&](int x0, int y0, int xSpan, int ySpan, VoxelId mat) {
            out.push_back(SurfaceQuad{
                .normal = Direction::PosZ,
                .cellMin = glm::ivec3(x0, y0, z),
                .span = glm::ivec2(xSpan, ySpan),
                .material = mat
            });
        });
        greedy2d(dims.x, dims.y, negMask, [&](int x0, int y0, int xSpan, int ySpan, VoxelId mat) {
            out.push_back(SurfaceQuad{
                .normal = Direction::NegZ,
                .cellMin = glm::ivec3(x0, y0, z),
                .span = glm::ivec2(xSpan, ySpan),
                .material = mat
            });
        });
    }
}

} // namespace Rigel::Voxel
