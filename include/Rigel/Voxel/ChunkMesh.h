#pragma once

/**
 * @file ChunkMesh.h
 * @brief CPU-side mesh data for chunk rendering.
 *
 * ChunkMesh stores vertex/index data and render-layer ranges. GPU resources
 * are managed by renderer-side caches.
 */

#include "VoxelVertex.h"
#include "Block.h"

#include <vector>
#include <array>

namespace Rigel::Voxel {

/**
 * @brief CPU mesh data for a single chunk.
 *
 * Holds vertices, indices, and per-layer index ranges.
 */
struct ChunkMesh {
    /// Vertex data
    std::vector<VoxelVertex> vertices;

    /// Index data (triangles)
    std::vector<uint32_t> indices;

    /**
     * @brief Index range for a render layer.
     */
    struct LayerRange {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;

        bool isEmpty() const { return indexCount == 0; }
    };

    /// Per-layer index ranges (Opaque, Cutout, Transparent, Emissive)
    std::array<LayerRange, RenderLayerCount> layers{};

    /**
     * @brief Check if mesh has no geometry.
     */
    bool isEmpty() const { return vertices.empty() || indices.empty(); }

    /**
     * @brief Get total vertex count.
     */
    size_t vertexCount() const { return vertices.size(); }

    /**
     * @brief Get total index count.
     */
    size_t indexCount() const { return indices.size(); }

    /**
     * @brief Get total triangle count.
     */
    size_t triangleCount() const { return indices.size() / 3; }

    ChunkMesh() = default;
};

} // namespace Rigel::Voxel
