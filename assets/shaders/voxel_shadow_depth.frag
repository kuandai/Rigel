#version 410 core

in vec2 v_uv;
flat in int v_textureLayer;

uniform sampler2DArray u_textureAtlas;
uniform float u_alphaCutoff;

void main() {
    if (u_alphaCutoff > 0.0) {
        float alpha = texture(u_textureAtlas, vec3(v_uv, float(v_textureLayer))).a;
        if (alpha < u_alphaCutoff) {
            discard;
        }
    }
}
