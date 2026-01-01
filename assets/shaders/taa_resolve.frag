#version 410 core

in vec2 v_uv;

uniform sampler2D u_currentColor;
uniform sampler2D u_currentDepth;
uniform sampler2D u_historyColor;
uniform sampler2D u_historyDepth;
uniform vec2 u_currentJitter;
uniform mat4 u_invViewProjection;
uniform mat4 u_prevViewProjection;
uniform float u_historyBlend;
uniform int u_historyValid;
uniform vec2 u_texelSize;

out vec4 fragColor;

vec3 sampleCurrent(vec2 uv) {
    return texture(u_currentColor, uv).rgb;
}

void main() {
    vec2 currUv = v_uv - u_currentJitter;
    vec3 current = sampleCurrent(currUv);
    float depth = texture(u_currentDepth, currUv).r;

    if (depth >= 1.0) {
        fragColor = vec4(current, 1.0);
        return;
    }

    vec4 clip = vec4(currUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = u_invViewProjection * clip;
    if (world.w <= 0.0) {
        fragColor = vec4(current, 1.0);
        return;
    }
    world /= world.w;

    vec4 prevClip = u_prevViewProjection * vec4(world.xyz, 1.0);
    if (prevClip.w <= 0.0) {
        fragColor = vec4(current, 1.0);
        return;
    }
    vec2 prevUv = prevClip.xy / prevClip.w * 0.5 + 0.5;

    bool validHistory = u_historyValid != 0 &&
        prevUv.x >= 0.0 && prevUv.x <= 1.0 &&
        prevUv.y >= 0.0 && prevUv.y <= 1.0;

    if (validHistory) {
        float prevDepth = texture(u_historyDepth, prevUv).r;
        float prevDepthClip = prevClip.z / prevClip.w;
        float prevDepth01 = prevDepthClip * 0.5 + 0.5;
        const float kDepthThreshold = 0.01;
        validHistory = prevDepth01 >= 0.0 && prevDepth01 <= 1.0 &&
            abs(prevDepth - prevDepth01) < kDepthThreshold;
    }

    vec3 history = current;
    if (validHistory) {
        history = texture(u_historyColor, prevUv).rgb;
    }

    vec3 minColor = current;
    vec3 maxColor = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * u_texelSize;
            vec3 neighbor = sampleCurrent(currUv + offset);
            minColor = min(minColor, neighbor);
            maxColor = max(maxColor, neighbor);
        }
    }

    history = clamp(history, minColor, maxColor);

    float historyWeight = validHistory ? clamp(u_historyBlend, 0.0, 1.0) : 0.0;
    vec3 color = mix(current, history, historyWeight);

    fragColor = vec4(color, 1.0);
}
