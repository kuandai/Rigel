#include "Rigel/Voxel/Noise.h"

#include <cmath>

namespace Rigel::Voxel::Noise {

namespace {
uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

float hashToUnit(uint64_t x) {
    uint64_t h = hash64(x);
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float valueNoise2D(int x, int z, uint32_t seed) {
    uint64_t key = (static_cast<uint64_t>(x) << 32) ^ static_cast<uint32_t>(z) ^ seed;
    return hashToUnit(key) * 2.0f - 1.0f;
}

uint32_t gradientHash(int x, int y, int z, uint32_t seed) {
    uint64_t key = (static_cast<uint64_t>(x) << 42)
        ^ (static_cast<uint64_t>(y) << 21)
        ^ static_cast<uint32_t>(z)
        ^ seed;
    return static_cast<uint32_t>(hash64(key));
}

float dotGrad(int gradIndex, float x, float y, float z) {
    static constexpr int kGradients[12][3] = {
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
        {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
        {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}
    };
    const int* g = kGradients[gradIndex % 12];
    return static_cast<float>(g[0]) * x + static_cast<float>(g[1]) * y + static_cast<float>(g[2]) * z;
}

float perlinNoise3D(float x, float y, float z, uint32_t seed) {
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int z0 = static_cast<int>(std::floor(z));
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    int z1 = z0 + 1;

    float fx = x - static_cast<float>(x0);
    float fy = y - static_cast<float>(y0);
    float fz = z - static_cast<float>(z0);

    float u = fade(fx);
    float v = fade(fy);
    float w = fade(fz);

    float n000 = dotGrad(static_cast<int>(gradientHash(x0, y0, z0, seed) & 0xFF), fx, fy, fz);
    float n100 = dotGrad(static_cast<int>(gradientHash(x1, y0, z0, seed) & 0xFF), fx - 1.0f, fy, fz);
    float n010 = dotGrad(static_cast<int>(gradientHash(x0, y1, z0, seed) & 0xFF), fx, fy - 1.0f, fz);
    float n110 = dotGrad(static_cast<int>(gradientHash(x1, y1, z0, seed) & 0xFF), fx - 1.0f, fy - 1.0f, fz);
    float n001 = dotGrad(static_cast<int>(gradientHash(x0, y0, z1, seed) & 0xFF), fx, fy, fz - 1.0f);
    float n101 = dotGrad(static_cast<int>(gradientHash(x1, y0, z1, seed) & 0xFF), fx - 1.0f, fy, fz - 1.0f);
    float n011 = dotGrad(static_cast<int>(gradientHash(x0, y1, z1, seed) & 0xFF), fx, fy - 1.0f, fz - 1.0f);
    float n111 = dotGrad(static_cast<int>(gradientHash(x1, y1, z1, seed) & 0xFF), fx - 1.0f, fy - 1.0f, fz - 1.0f);

    float x00 = lerp(n000, n100, u);
    float x10 = lerp(n010, n110, u);
    float x01 = lerp(n001, n101, u);
    float x11 = lerp(n011, n111, u);
    float yBlend0 = lerp(x00, x10, v);
    float yBlend1 = lerp(x01, x11, v);
    return lerp(yBlend0, yBlend1, w);
}
} // namespace

uint32_t seedForChannel(uint32_t baseSeed, std::string_view name) {
    uint32_t hash = 2166136261u;
    for (char c : name) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return baseSeed ^ hash;
}

float noise2D(float x, float z, uint32_t seed) {
    int x0 = static_cast<int>(std::floor(x));
    int z0 = static_cast<int>(std::floor(z));
    int x1 = x0 + 1;
    int z1 = z0 + 1;

    float fx = x - static_cast<float>(x0);
    float fz = z - static_cast<float>(z0);

    float v00 = valueNoise2D(x0, z0, seed);
    float v10 = valueNoise2D(x1, z0, seed);
    float v01 = valueNoise2D(x0, z1, seed);
    float v11 = valueNoise2D(x1, z1, seed);

    float tx = smoothstep(fx);
    float tz = smoothstep(fz);
    float a = lerp(v00, v10, tx);
    float b = lerp(v01, v11, tx);
    return lerp(a, b, tz);
}

float noise3D(float x, float y, float z, uint32_t seed) {
    return perlinNoise3D(x, y, z, seed);
}

float fbm2D(float x, float z, uint32_t seed, const WorldGenConfig::NoiseConfig& config) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = config.frequency;
    float maxValue = 0.0f;

    for (int i = 0; i < config.octaves; ++i) {
        total += noise2D(x * frequency, z * frequency, seed + static_cast<uint32_t>(i)) * amplitude;
        maxValue += amplitude;
        amplitude *= config.persistence;
        frequency *= config.lacunarity;
    }

    if (maxValue > 0.0f) {
        total /= maxValue;
    }

    return total * config.scale + config.offset;
}

float fbm3D(float x, float y, float z, uint32_t seed, const WorldGenConfig::NoiseConfig& config) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = config.frequency;
    float maxValue = 0.0f;

    for (int i = 0; i < config.octaves; ++i) {
        total += noise3D(x * frequency, y * frequency, z * frequency, seed + static_cast<uint32_t>(i))
            * amplitude;
        maxValue += amplitude;
        amplitude *= config.persistence;
        frequency *= config.lacunarity;
    }

    if (maxValue > 0.0f) {
        total /= maxValue;
    }

    return total * config.scale + config.offset;
}

} // namespace Rigel::Voxel::Noise
