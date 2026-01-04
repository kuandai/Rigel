#pragma once

#include "Rigel/Voxel/WorldGenConfig.h"

namespace Rigel::Voxel {

class WorldGenerator;

int findFirstAirY(const WorldGenerator& generator,
                  const WorldGenConfig& config,
                  int worldX,
                  int worldZ);

} // namespace Rigel::Voxel
