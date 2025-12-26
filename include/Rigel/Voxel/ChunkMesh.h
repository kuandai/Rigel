#pragma once

/**
 * @file ChunkMesh.h
 * @brief GPU mesh representation for chunk rendering.
 *
 * ChunkMesh holds both the vertex/index data and the OpenGL buffer objects
 * needed to render a chunk. It supports multiple render layers for proper
 * transparency handling.
 */

#include "VoxelVertex.h"
#include "Block.h"

#include <vector>
#include <array>
#include <GL/glew.h>

namespace Rigel::Voxel {

/**
 * @brief GPU-ready mesh for a single chunk.
 *
 * Manages vertex and index data along with OpenGL buffer objects.
 * Supports layer-based rendering for opaque/transparent separation.
 *
 * @section lifecycle Lifecycle
 *
 * 1. MeshBuilder populates vertices and indices
 * 2. Call uploadToGPU() to create VAO/VBO/EBO
 * 3. Use draw() or drawLayer() for rendering
 * 4. Destructor or releaseGPU() cleans up OpenGL resources
 *
 * @note Requires a valid OpenGL context for GPU operations.
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

    /// @name OpenGL Resources
    /// @{
    GLuint vao = 0;  ///< Vertex Array Object
    GLuint vbo = 0;  ///< Vertex Buffer Object
    GLuint ebo = 0;  ///< Element Buffer Object (indices)
    /// @}

    ChunkMesh() = default;

    /**
     * @brief Destructor. Releases GPU resources.
     */
    ~ChunkMesh();

    /// Non-copyable (owns GPU resources)
    ChunkMesh(const ChunkMesh&) = delete;
    ChunkMesh& operator=(const ChunkMesh&) = delete;

    /// Movable
    ChunkMesh(ChunkMesh&& other) noexcept;
    ChunkMesh& operator=(ChunkMesh&& other) noexcept;

    /**
     * @brief Upload vertex and index data to GPU.
     *
     * Creates VAO, VBO, and EBO if they don't exist.
     * Re-uploads data if buffers already exist.
     */
    void uploadToGPU();

    /**
     * @brief Release GPU resources.
     *
     * Deletes VAO, VBO, and EBO. Safe to call multiple times.
     */
    void releaseGPU();

    /**
     * @brief Check if mesh has GPU resources allocated.
     */
    bool isUploaded() const { return vao != 0; }

    /**
     * @brief Check if mesh has no geometry.
     */
    bool isEmpty() const { return vertices.empty(); }

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

    /**
     * @brief Draw all indices.
     *
     * Binds the VAO and issues a draw call for all indices.
     * Assumes shader is already bound.
     */
    void draw() const;

    /**
     * @brief Draw a specific render layer.
     *
     * @param layer The layer to draw
     *
     * Binds the VAO and issues a draw call for the layer's index range.
     * Does nothing if the layer has no indices.
     */
    void drawLayer(RenderLayer layer) const;

    /**
     * @brief Clear CPU-side data after GPU upload.
     *
     * Call this to free CPU memory after uploadToGPU().
     * The mesh can still be rendered but not re-uploaded.
     */
    void clearCPUData();
};

} // namespace Rigel::Voxel
