#pragma once

namespace Rigel::Voxel {

class WorldGenerator;

int findFirstAirY(const WorldGenerator& generator,
                  int worldX,
                  int worldZ);

} // namespace Rigel::Voxel
