#version 410 core

// Vertex attributes
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_packedData;  // normalIndex, aoLevel, textureLayer, flags

// Uniforms
uniform mat4 u_viewProjection;
uniform vec3 u_chunkOffset;

// Outputs to fragment shader
out vec2 v_uv;
out float v_ao;
out vec3 v_normal;
flat out int v_textureLayer;

// Normal lookup table for axis-aligned faces
const vec3 NORMALS[6] = vec3[](
    vec3( 1.0,  0.0,  0.0),  // PosX
    vec3(-1.0,  0.0,  0.0),  // NegX
    vec3( 0.0,  1.0,  0.0),  // PosY
    vec3( 0.0, -1.0,  0.0),  // NegY
    vec3( 0.0,  0.0,  1.0),  // PosZ
    vec3( 0.0,  0.0, -1.0)   // NegZ
);

void main() {
    // Calculate world position
    vec3 worldPos = a_position + u_chunkOffset;
    gl_Position = u_viewProjection * vec4(worldPos, 1.0);

    // Pass through UV coordinates
    v_uv = a_uv;

    // Unpack data
    int normalIndex = int(a_packedData.x);
    v_ao = a_packedData.y / 3.0;  // 0-3 -> 0-1
    v_textureLayer = int(a_packedData.z);

    // Look up normal from table
    v_normal = NORMALS[clamp(normalIndex, 0, 5)];
}
