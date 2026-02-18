#include "Rigel/Voxel/VoxelLod/VoxelMipPyramid.h"

#include <algorithm>
#include <array>

namespace Rigel::Voxel {
namespace {

bool isPow2(int v) {
    return v > 0 && (v & (v - 1)) == 0;
}

size_t cellIndex(int x, int y, int z, int dim) {
    return static_cast<size_t>(x)
        + static_cast<size_t>(y) * static_cast<size_t>(dim)
        + static_cast<size_t>(z) * static_cast<size_t>(dim) * static_cast<size_t>(dim);
}

VoxelId dominantValue(std::array<VoxelId, 8> values) {
    VoxelId best = values[0];
    int bestCount = 1;

    for (size_t i = 0; i < values.size(); ++i) {
        int count = 0;
        for (size_t j = 0; j < values.size(); ++j) {
            if (values[j] == values[i]) {
                ++count;
            }
        }
        if (count > bestCount) {
            bestCount = count;
            best = values[i];
        }
    }
    return best;
}

} // namespace

VoxelMipPyramid buildVoxelMipPyramid(std::span<const VoxelId> l0, int baseDim) {
    VoxelMipPyramid out;
    if (baseDim <= 0 || !isPow2(baseDim)) {
        return out;
    }
    const size_t expected = static_cast<size_t>(baseDim) *
        static_cast<size_t>(baseDim) *
        static_cast<size_t>(baseDim);
    if (l0.size() != expected) {
        return out;
    }

    out.baseDim = baseDim;

    VoxelMipLevel level0;
    level0.dim = baseDim;
    level0.cells.resize(expected);
    for (size_t i = 0; i < expected; ++i) {
        level0.cells[i] = VoxelMipLevel::pack(true, l0[i]);
    }
    out.levels.push_back(std::move(level0));

    int prevDim = baseDim;
    while (prevDim > 1) {
        const VoxelMipLevel& prev = out.levels.back();
        const int nextDim = prevDim / 2;
        VoxelMipLevel next;
        next.dim = nextDim;
        next.cells.resize(static_cast<size_t>(nextDim) *
                          static_cast<size_t>(nextDim) *
                          static_cast<size_t>(nextDim));

        for (int z = 0; z < nextDim; ++z) {
            for (int y = 0; y < nextDim; ++y) {
                for (int x = 0; x < nextDim; ++x) {
                    const int bx = x * 2;
                    const int by = y * 2;
                    const int bz = z * 2;
                    std::array<uint32_t, 8> childPacked{
                        prev.cells[cellIndex(bx + 0, by + 0, bz + 0, prevDim)],
                        prev.cells[cellIndex(bx + 1, by + 0, bz + 0, prevDim)],
                        prev.cells[cellIndex(bx + 0, by + 1, bz + 0, prevDim)],
                        prev.cells[cellIndex(bx + 1, by + 1, bz + 0, prevDim)],
                        prev.cells[cellIndex(bx + 0, by + 0, bz + 1, prevDim)],
                        prev.cells[cellIndex(bx + 1, by + 0, bz + 1, prevDim)],
                        prev.cells[cellIndex(bx + 0, by + 1, bz + 1, prevDim)],
                        prev.cells[cellIndex(bx + 1, by + 1, bz + 1, prevDim)],
                    };

                    std::array<VoxelId, 8> childValues{};
                    bool allUniform = true;
                    for (size_t i = 0; i < childPacked.size(); ++i) {
                        allUniform = allUniform && VoxelMipLevel::isUniform(childPacked[i]);
                        childValues[i] = VoxelMipLevel::value(childPacked[i]);
                    }

                    const bool sameValue =
                        std::all_of(childValues.begin() + 1, childValues.end(),
                                    [&](VoxelId v) { return v == childValues[0]; });

                    const bool uniform = allUniform && sameValue;
                    const VoxelId value = uniform ? childValues[0] : dominantValue(childValues);
                    next.cells[cellIndex(x, y, z, nextDim)] = VoxelMipLevel::pack(uniform, value);
                }
            }
        }

        out.levels.push_back(std::move(next));
        prevDim = nextDim;
    }

    return out;
}

} // namespace Rigel::Voxel

