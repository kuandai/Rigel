#version 410 core

// Inputs from vertex shader
in vec2 v_uv;
in float v_ao;
in vec3 v_normal;
flat in int v_textureLayer;

// Uniforms
uniform sampler2DArray u_textureAtlas;
uniform vec3 u_sunDirection;

// Output
out vec4 fragColor;

void main() {
    // Sample texture from array
    vec4 texColor = texture(u_textureAtlas, vec3(v_uv, float(v_textureLayer)));

    // Alpha test for cutout materials
    if (texColor.a < 0.5) {
        discard;
    }

    // Simple directional lighting
    float diffuse = max(dot(v_normal, u_sunDirection), 0.0);
    float lighting = 0.3 + 0.7 * diffuse;  // Ambient + diffuse

    // Apply ambient occlusion
    float ao = 0.5 + 0.5 * v_ao;

    // Final color
    vec3 finalColor = texColor.rgb * lighting * ao;
    fragColor = vec4(finalColor, texColor.a);
}
