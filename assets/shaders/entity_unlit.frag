#version 410 core

in vec2 v_uv;

uniform sampler2D u_diffuse;
uniform sampler2D u_emission;
uniform int u_hasEmission;
uniform vec4 u_tintColor;

out vec4 fragColor;

void main() {
    vec4 texColor = texture(u_diffuse, v_uv);
    if (texColor.a <= 0.0) {
        discard;
    }

    vec4 base = texColor * u_tintColor;
    vec3 emission = vec3(0.0);
    if (u_hasEmission != 0) {
        vec4 emissionTex = texture(u_emission, v_uv);
        emission = emissionTex.rgb * emissionTex.a;
    }

    vec3 color = max(base.rgb, emission);
    fragColor = vec4(color, base.a);
}
