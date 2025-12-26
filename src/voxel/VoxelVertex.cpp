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

    // Packed data: location 2, vec4 (as normalized ubyte)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(
        2,                              // location
        4,                              // size (normalIndex, aoLevel, textureLayer, flags)
        GL_UNSIGNED_BYTE,               // type
        GL_FALSE,                       // normalized (we'll handle in shader)
        sizeof(VoxelVertex),            // stride
        reinterpret_cast<void*>(offsetof(VoxelVertex, normalIndex))
    );
}

} // namespace Rigel::Voxel
