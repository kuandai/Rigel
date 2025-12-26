#include "Rigel/Voxel/ChunkMesh.h"

namespace Rigel::Voxel {

ChunkMesh::~ChunkMesh() {
    releaseGPU();
}

ChunkMesh::ChunkMesh(ChunkMesh&& other) noexcept
    : vertices(std::move(other.vertices))
    , indices(std::move(other.indices))
    , layers(other.layers)
    , vao(other.vao)
    , vbo(other.vbo)
    , ebo(other.ebo)
{
    other.vao = 0;
    other.vbo = 0;
    other.ebo = 0;
}

ChunkMesh& ChunkMesh::operator=(ChunkMesh&& other) noexcept {
    if (this != &other) {
        releaseGPU();

        vertices = std::move(other.vertices);
        indices = std::move(other.indices);
        layers = other.layers;
        vao = other.vao;
        vbo = other.vbo;
        ebo = other.ebo;

        other.vao = 0;
        other.vbo = 0;
        other.ebo = 0;
    }
    return *this;
}

void ChunkMesh::uploadToGPU() {
    if (vertices.empty()) {
        return;
    }

    // Create buffers if needed
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
    }

    glBindVertexArray(vao);

    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(vertices.size() * sizeof(VoxelVertex)),
        vertices.data(),
        GL_STATIC_DRAW
    );

    // Setup vertex attributes
    VoxelVertex::setupAttributes();

    // Upload index data
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
        indices.data(),
        GL_STATIC_DRAW
    );

    glBindVertexArray(0);
}

void ChunkMesh::releaseGPU() {
    if (vao != 0) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    if (vbo != 0) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (ebo != 0) {
        glDeleteBuffers(1, &ebo);
        ebo = 0;
    }
}

void ChunkMesh::draw() const {
    if (vao == 0 || indices.empty()) {
        return;
    }

    glBindVertexArray(vao);
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(indices.size()),
        GL_UNSIGNED_INT,
        nullptr
    );
    glBindVertexArray(0);
}

void ChunkMesh::drawLayer(RenderLayer layer) const {
    if (vao == 0) {
        return;
    }

    const LayerRange& range = layers[static_cast<size_t>(layer)];
    if (range.isEmpty()) {
        return;
    }

    glBindVertexArray(vao);
    glDrawElements(
        GL_TRIANGLES,
        static_cast<GLsizei>(range.indexCount),
        GL_UNSIGNED_INT,
        reinterpret_cast<void*>(static_cast<uintptr_t>(range.indexStart * sizeof(uint32_t)))
    );
    glBindVertexArray(0);
}

void ChunkMesh::clearCPUData() {
    vertices.clear();
    vertices.shrink_to_fit();
    indices.clear();
    indices.shrink_to_fit();
}

} // namespace Rigel::Voxel
