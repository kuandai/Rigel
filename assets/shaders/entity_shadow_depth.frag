#version 410 core

in vec2 v_uv;

uniform sampler2D u_diffuse;
uniform int u_useAlphaTest;

void main() {
    if (u_useAlphaTest != 0) {
        float alpha = texture(u_diffuse, v_uv).a;
        if (alpha <= 0.0) {
            discard;
        }
    }
}
