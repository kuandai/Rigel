#version 410 core

layout(location = 0) in vec3 a_position;

uniform mat4 u_lightViewProjection;
uniform mat4 u_model;

void main() {
    gl_Position = u_lightViewProjection * u_model * vec4(a_position, 1.0);
}
