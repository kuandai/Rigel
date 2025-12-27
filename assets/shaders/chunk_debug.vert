#version 410 core

layout(location = 0) in vec3 a_position;

uniform mat4 u_viewProjection;
uniform vec3 u_fieldOrigin;
uniform vec3 u_fieldRight;
uniform vec3 u_fieldUp;
uniform vec3 u_fieldForward;
uniform float u_cellSize;
uniform vec4 u_color;

out vec4 v_color;

void main() {
    vec3 worldPos = u_fieldOrigin
        + u_fieldRight * (a_position.x * u_cellSize)
        + u_fieldUp * (a_position.y * u_cellSize)
        + u_fieldForward * (a_position.z * u_cellSize);
    vec3 pos = worldPos;
    gl_Position = u_viewProjection * vec4(pos, 1.0);
    v_color = u_color;
}
