#version 410 core

layout(location = 0) in vec2 a_position;

uniform vec4 u_color;

out vec4 v_color;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = u_color;
}
