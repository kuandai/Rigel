#pragma once

#include <array>

namespace Rigel::Voxel {

inline constexpr std::array<const char*, 7> kWorldGenPipelineStages = {
    "climate_global",
    "climate_local",
    "biome_resolve",
    "terrain_density",
    "caves",
    "surface_rules",
    "structures"
};

} // namespace Rigel::Voxel
