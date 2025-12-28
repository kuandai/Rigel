#pragma once

#include "WorldGenConfig.h"

#include <cstdint>
#include <string_view>

namespace Rigel::Voxel::Noise {

uint32_t seedForChannel(uint32_t baseSeed, std::string_view name);

float noise2D(float x, float z, uint32_t seed);
float noise3D(float x, float y, float z, uint32_t seed);

float fbm2D(float x, float z, uint32_t seed, const WorldGenConfig::NoiseConfig& config);
float fbm3D(float x, float y, float z, uint32_t seed, const WorldGenConfig::NoiseConfig& config);

}
