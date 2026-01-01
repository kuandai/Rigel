#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_viewProjection;
uniform mat4 u_view;
uniform mat4 u_model;

out vec2 v_uv;
out vec3 v_normal;
out vec3 v_worldPos;
out float v_viewDepth;

void main() {
    vec4 worldPos = u_model * vec4(a_position, 1.0);
    vec4 viewPos = u_view * worldPos;
    v_worldPos = worldPos.xyz;
    mat3 normalMat = transpose(inverse(mat3(u_model)));
    v_normal = normalize(normalMat * a_normal);
    v_uv = a_uv;
    gl_Position = u_viewProjection * worldPos;
    v_viewDepth = -viewPos.z;
}
