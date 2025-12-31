#version 410 core

flat in int v_textureLayer;

uniform sampler2DArray u_shadowTintAtlas;
uniform float u_transparentScale;

out vec4 fragColor;

void main() {
    vec4 tintSample = texture(u_shadowTintAtlas, vec3(0.5, 0.5, float(v_textureLayer)));
    float alpha = clamp(tintSample.a * u_transparentScale, 0.0, 1.0);
    vec3 tint = mix(vec3(1.0), tintSample.rgb, alpha);
    fragColor = vec4(tint, 1.0);
}
