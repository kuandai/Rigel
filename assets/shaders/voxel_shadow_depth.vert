#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_packedData;  // normalIndex, aoLevel, textureLayer, flags

uniform mat4 u_lightViewProjection;
uniform vec3 u_chunkOffset;

out vec2 v_uv;
flat out int v_textureLayer;

void main() {
    vec3 worldPos = a_position + u_chunkOffset;
    gl_Position = u_lightViewProjection * vec4(worldPos, 1.0);
    v_uv = a_uv;
    v_textureLayer = int(a_packedData.z);
}
