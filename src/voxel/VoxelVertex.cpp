#include "Rigel/Voxel/VoxelVertex.h"

namespace Rigel::Voxel {

void VoxelVertex::setupAttributes() {
    // Position: location 0, vec3
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,                              // location
        3,                              // size (x, y, z)
        GL_FLOAT,                       // type
        GL_FALSE,                       // normalized
        sizeof(VoxelVertex),            // stride
        reinterpret_cast<void*>(offsetof(VoxelVertex, x))
    );

    // UV: location 1, vec2
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1,                              // location
        2,                              // size (u, v)
        GL_FLOAT,                       // type
        GL_FALSE,                       // normalized
        sizeof(VoxelVertex),            // stride
        reinterpret_cast<void*>(offsetof(VoxelVertex, u))
    );

    // Face data: location 2, uvec2
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(
        2,                              // location
        2,                              // size (normalIndex, aoLevel)
        GL_UNSIGNED_BYTE,               // type
        sizeof(VoxelVertex),            // stride
        reinterpret_cast<void*>(offsetof(VoxelVertex, normalIndex))
    );

    // Texture layer: location 3, uint sourced from an unsigned short
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(
        3,                              // location
        1,                              // size (textureLayer)
        GL_UNSIGNED_SHORT,              // type
        sizeof(VoxelVertex),            // stride
        reinterpret_cast<void*>(offsetof(VoxelVertex, textureLayer))
    );
}

} // namespace Rigel::Voxel
