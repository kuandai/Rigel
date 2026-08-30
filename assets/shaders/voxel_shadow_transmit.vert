#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in uvec2 a_faceData;  // normalIndex, aoLevel
layout(location = 3) in uint a_textureLayer;

uniform mat4 u_lightViewProjection;
uniform vec3 u_chunkOffset;

flat out int v_textureLayer;

void main() {
    vec3 worldPos = a_position + u_chunkOffset;
    gl_Position = u_lightViewProjection * vec4(worldPos, 1.0);
    v_textureLayer = int(a_textureLayer);
}
