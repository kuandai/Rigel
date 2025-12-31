#version 410 core

// Inputs from vertex shader
in vec2 v_uv;
in float v_ao;
in vec3 v_normal;
in vec3 v_worldPos;
in float v_viewDepth;
flat in int v_textureLayer;

// Uniforms
uniform sampler2DArray u_textureAtlas;
uniform vec3 u_sunDirection;
uniform float u_alphaMultiplier;
uniform float u_alphaCutoff;
uniform int u_renderLayer; // 0=opaque, 1=cutout, 2=transparent, 3=emissive
uniform int u_shadowEnabled;
uniform sampler2DArray u_shadowMap;
uniform sampler2DArray u_shadowTransmittanceMap;
uniform mat4 u_shadowMatrices[4];
uniform float u_shadowSplits[4];
uniform int u_shadowCascadeCount;
uniform float u_shadowBias;
uniform float u_shadowNormalBias;
uniform int u_shadowPcfRadius;
uniform float u_shadowPcfNear;
uniform float u_shadowPcfFar;
uniform float u_shadowStrength;
uniform float u_shadowNear;
uniform float u_shadowFadeStart;
uniform float u_shadowFadePower;

// Output
out vec4 fragColor;

int selectCascade(float viewDepth) {
    int cascade = 0;
    for (int i = 0; i < u_shadowCascadeCount; ++i) {
        if (viewDepth <= u_shadowSplits[i]) {
            cascade = i;
            return cascade;
        }
    }
    return max(u_shadowCascadeCount - 1, 0);
}

float computePcfRadius(float viewDepth) {
    int lastCascade = max(u_shadowCascadeCount - 1, 0);
    float nearPlane = max(u_shadowNear, 0.0001);
    float farPlane = max(u_shadowSplits[lastCascade], nearPlane + 0.0001);
    float denom = log(max(farPlane / nearPlane, 1.0001));
    float depthClamped = clamp(viewDepth, nearPlane, farPlane);
    float t = (denom > 0.0) ? (log(depthClamped / nearPlane) / denom) : 0.0;
    float radius = mix(u_shadowPcfNear, u_shadowPcfFar, clamp(t, 0.0, 1.0));
    radius = clamp(radius, 0.0, 4.0);
    return radius;
}

float sampleShadowMap(vec4 shadowPos, int layer, float bias, float radius) {
    vec3 proj = shadowPos.xyz / shadowPos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) {
        return 1.0;
    }

    ivec3 mapSize = textureSize(u_shadowMap, 0);
    vec2 texel = 1.0 / vec2(mapSize.xy);
    float shadow = 0.0;
    float totalWeight = 0.0;
    float radiusClamped = max(radius, 0.0);
    int radiusCeil = int(ceil(radiusClamped));
    for (int y = -radiusCeil; y <= radiusCeil; ++y) {
        for (int x = -radiusCeil; x <= radiusCeil; ++x) {
            vec2 offset = vec2(float(x), float(y)) * texel;
            float depth = texture(u_shadowMap, vec3(proj.xy + offset, float(layer))).r;
            float weightX = max(radiusClamped + 1.0 - abs(float(x)), 0.0);
            float weightY = max(radiusClamped + 1.0 - abs(float(y)), 0.0);
            float weight = weightX * weightY;
            shadow += ((proj.z - bias > depth) ? 0.0 : 1.0) * weight;
            totalWeight += weight;
        }
    }
    if (totalWeight > 0.0) {
        shadow /= totalWeight;
    }
    return shadow;
}

vec3 sampleShadowColor(int cascade, float diffuse, float radius) {
    vec4 shadowPos = u_shadowMatrices[cascade] * vec4(v_worldPos, 1.0);
    float bias = u_shadowBias + u_shadowNormalBias * (1.0 - diffuse);
    float shadowFactor = sampleShadowMap(shadowPos, cascade, bias, radius);
    shadowFactor = pow(shadowFactor, max(u_shadowStrength, 0.0));
    vec3 shadowTint = vec3(1.0);
    vec3 proj = shadowPos.xyz / shadowPos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z <= 1.0) {
        shadowTint = texture(u_shadowTransmittanceMap,
                             vec3(proj.xy, float(cascade))).rgb;
    }
    return shadowTint * shadowFactor;
}

void main() {
    // Sample texture from array
    vec4 texColor = texture(u_textureAtlas, vec3(v_uv, float(v_textureLayer)));
    float alpha = texColor.a * u_alphaMultiplier;

    // Alpha test for cutout materials
    if (alpha < u_alphaCutoff) {
        discard;
    }

    // Simple directional lighting
    float diffuse = max(dot(v_normal, u_sunDirection), 0.0);
    float ambient = 0.3;
    float sun = 0.7 * diffuse;

    vec3 shadowColor = vec3(1.0);
    if (u_shadowEnabled != 0 && u_shadowCascadeCount > 0 && u_renderLayer != 2) {
        int cascade = selectCascade(v_viewDepth);
        float radius = computePcfRadius(v_viewDepth);
        shadowColor = sampleShadowColor(cascade, diffuse, radius);

        if (cascade + 1 < u_shadowCascadeCount) {
            float split = u_shadowSplits[cascade];
            float prevSplit = (cascade == 0) ? u_shadowNear : u_shadowSplits[cascade - 1];
            float base = max(prevSplit, 0.0001);
            float denom = log(max(split / base, 1.0001));
            float depthClamped = clamp(v_viewDepth, base, split);
            float t = (denom > 0.0) ? (log(depthClamped / base) / denom) : 0.0;
            if (t > 0.0) {
                vec3 nextColor = sampleShadowColor(cascade + 1, diffuse, radius);
                shadowColor = mix(shadowColor, nextColor, clamp(t, 0.0, 1.0));
            }
        }

        int lastCascade = max(u_shadowCascadeCount - 1, 0);
        float shadowFar = u_shadowSplits[lastCascade];
        float fadeStart = min(u_shadowFadeStart, shadowFar);
        if (fadeStart > 0.0 && fadeStart < shadowFar && v_viewDepth > fadeStart) {
            float base = max(fadeStart, 0.0001);
            float denom = log(max(shadowFar / base, 1.0001));
            float depthClamped = clamp(v_viewDepth, base, shadowFar);
            float t = (denom > 0.0) ? (log(depthClamped / base) / denom) : 1.0;
            t = clamp(t, 0.0, 1.0);
            t = pow(t, max(u_shadowFadePower, 0.0));
            float fade = clamp(1.0 - t, 0.0, 1.0);
            shadowColor = mix(vec3(1.0), shadowColor, fade);
        }
    }

    // Apply ambient occlusion
    float ao = 0.5 + 0.5 * v_ao;

    // Final color
    vec3 finalColor = texColor.rgb * (ambient + sun * shadowColor) * ao;
    fragColor = vec4(finalColor, alpha);
}
