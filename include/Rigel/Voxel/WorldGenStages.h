#pragma once

#include <array>

namespace Rigel::Voxel {

inline constexpr std::array<const char*, 8> kWorldGenPipelineStages = {
    "climate_global",
    "climate_local",
    "biome_resolve",
    "terrain_density",
    "caves",
    "surface_rules",
    "structures",
    "post_process"
};

} // namespace Rigel::Voxel
