#pragma once

#include "Rigel/Voxel/Block.h"
#include "Rigel/Voxel/ChunkMesh.h"
#include "Rigel/Voxel/VoxelLod/VoxelSurfaceExtraction.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Rigel::Voxel {

// Build a ChunkMesh (opaque-only for now) from macro surface quads.
//
// Vertex positions are emitted in *page-local voxel coordinates*; the caller is
// expected to supply the page world offset as the voxel shader's u_chunkOffset.
//
// `faceTextureLayersByVoxelId` maps voxelId -> per-face atlas layer index.
ChunkMesh buildSurfaceMeshFromQuads(
    std::span<const SurfaceQuad> quads,
    int cellSizeVoxels,
    std::span<const std::array<uint16_t, DirectionCount>> faceTextureLayersByVoxelId);

} // namespace Rigel::Voxel

