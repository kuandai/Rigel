#pragma once

#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/VoxelLod/VoxelPageCpu.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Rigel::Voxel {

enum class VoxelBoundaryPolicy : uint8_t {
    OutsideEmpty = 0,
    OutsideSolid = 1
};

struct MacroVoxelGrid {
    glm::ivec3 dims{0};
    int cellSizeVoxels = 1; // how many L0 voxels one macro cell represents
    std::vector<VoxelId> cells; // dims.x * dims.y * dims.z

    bool empty() const {
        return dims.x <= 0 || dims.y <= 0 || dims.z <= 0 || cells.empty();
    }

    size_t cellCount() const {
        if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) {
            return 0;
        }
        return static_cast<size_t>(dims.x) * static_cast<size_t>(dims.y) * static_cast<size_t>(dims.z);
    }
};

struct SurfaceQuad {
    Direction normal = Direction::PosY;
    glm::ivec3 cellMin{0};   // macro-cell coordinate
    glm::ivec2 span{1, 1};   // span in macro cells (u,v) for the face plane
    VoxelId material = kVoxelAir;
};

struct MacroVoxelNeighbors {
    const MacroVoxelGrid* negX = nullptr;
    const MacroVoxelGrid* posX = nullptr;
    const MacroVoxelGrid* negY = nullptr;
    const MacroVoxelGrid* posY = nullptr;
    const MacroVoxelGrid* negZ = nullptr;
    const MacroVoxelGrid* posZ = nullptr;
};

// Build a macro-voxel grid from a page mip pyramid.
//
// The output grid has dimensions (page.dim / cellSizeVoxels)^3 and one VoxelId per macro cell.
// Non-uniform mip cells use the mip representative value, which intentionally "fills holes"
// at far resolution.
MacroVoxelGrid buildMacroGridFromPage(const VoxelPageCpu& page, int cellSizeVoxels);

// Extract surface quads at macro-voxel resolution.
//
// Faces are emitted only at solid-to-empty boundaries (material != 0 adjacent to 0).
// `boundaryPolicy` controls how the outer boundary of the grid is treated.
void extractSurfaceQuads(const MacroVoxelGrid& grid,
                         VoxelBoundaryPolicy boundaryPolicy,
                         std::vector<SurfaceQuad>& out);

// Same as extractSurfaceQuads(), but performs per-plane greedy merging so coplanar
// adjacent faces of the same material collapse into larger quads.
void extractSurfaceQuadsGreedy(const MacroVoxelGrid& grid,
                               VoxelBoundaryPolicy boundaryPolicy,
                               std::vector<SurfaceQuad>& out);

// Greedy surface extraction with a 6-neighbor cross for boundary sampling.
//
// This avoids double-faces between adjacent grids by sampling neighbor macro cells
// when a face query crosses the grid boundary. Missing neighbors are treated as
// either air or solid based on `boundaryPolicy`.
void extractSurfaceQuadsGreedy(const MacroVoxelGrid& grid,
                               const MacroVoxelNeighbors& neighbors,
                               VoxelBoundaryPolicy boundaryPolicy,
                               std::vector<SurfaceQuad>& out);

} // namespace Rigel::Voxel
