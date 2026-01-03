#version 410 core

in vec2 v_uv;
in vec3 v_normal;
in vec3 v_worldPos;

uniform sampler2D u_diffuse;
uniform sampler2D u_emission;
uniform int u_hasEmission;
uniform vec4 u_tintColor;
uniform vec3 u_sunDirection;
uniform float u_ambientStrength;
uniform float u_ao;
uniform int u_shadowEnabled;
uniform sampler2DArray u_shadowMap;
uniform sampler2DArray u_shadowTransmittanceMap;
uniform mat4 u_shadowMatrices[4];
uniform float u_shadowSplits[4];
uniform int u_shadowCascadeCount;
uniform float u_shadowBias;
uniform float u_shadowNormalBias;
uniform float u_shadowPcfNear;
uniform float u_shadowPcfFar;
uniform float u_shadowStrength;
uniform float u_shadowNear;
uniform float u_shadowFadeStart;
uniform float u_shadowFadePower;
uniform vec3 u_cameraPos;

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
    float maxRadius = max(u_shadowPcfNear, u_shadowPcfFar);
    maxRadius = clamp(maxRadius, 0.0, 4.0);
    int radiusCeil = int(ceil(maxRadius));
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
    vec4 texColor = texture(u_diffuse, v_uv);
    vec3 baseColor = texColor.rgb * u_tintColor.rgb;

    float diffuse = max(dot(normalize(v_normal), normalize(u_sunDirection)), 0.0);
    float ambient = clamp(u_ambientStrength, 0.0, 1.0);
    float sun = (1.0 - ambient) * diffuse;
    float viewDistance = length(v_worldPos - u_cameraPos);

    vec3 shadowColor = vec3(1.0);
    if (u_shadowEnabled != 0 && u_shadowCascadeCount > 0) {
        int cascade = selectCascade(viewDistance);
        float radius = computePcfRadius(viewDistance);
        shadowColor = sampleShadowColor(cascade, diffuse, radius);

        if (cascade + 1 < u_shadowCascadeCount) {
            float split = u_shadowSplits[cascade];
            float prevSplit = (cascade == 0) ? u_shadowNear : u_shadowSplits[cascade - 1];
            float blendSpan = max((split - prevSplit) * 0.1, 2.0);
            float t = clamp((viewDistance - (split - blendSpan)) / blendSpan, 0.0, 1.0);
            if (t > 0.0) {
                vec3 nextColor = sampleShadowColor(cascade + 1, diffuse, radius);
                shadowColor = mix(shadowColor, nextColor, t);
            }
        }

        int lastCascade = max(u_shadowCascadeCount - 1, 0);
        float shadowFar = u_shadowSplits[lastCascade];
        float fadeStart = min(u_shadowFadeStart, shadowFar);
        if (fadeStart > 0.0 && fadeStart < shadowFar && viewDistance > fadeStart) {
            float base = max(fadeStart, 0.0001);
            float denom = log(max(shadowFar / base, 1.0001));
            float depthClamped = clamp(viewDistance, base, shadowFar);
            float t = (denom > 0.0) ? (log(depthClamped / base) / denom) : 1.0;
            t = clamp(t, 0.0, 1.0);
            t = pow(t, max(u_shadowFadePower, 0.0));
            float fade = clamp(1.0 - t, 0.0, 1.0);
            shadowColor = mix(vec3(1.0), shadowColor, fade);
        }
    }

    float ao = clamp(u_ao, 0.0, 1.0);
    vec3 lit = baseColor * (ambient + sun * shadowColor) * ao;

    vec3 emission = vec3(0.0);
    if (u_hasEmission != 0) {
        emission = texture(u_emission, v_uv).rgb;
    }

    fragColor = vec4(lit + emission, texColor.a * u_tintColor.a);
}
