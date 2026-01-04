#include "Rigel/Voxel/WorldSpawn.h"

#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/WorldGenerator.h"

namespace Rigel::Voxel {

int findFirstAirY(const WorldGenerator& generator,
                  const WorldGenConfig& config,
                  int worldX,
                  int worldZ) {
    int minY = config.world.minY;
    int maxY = config.world.maxY;

    Voxel::ChunkCoord baseCoord = Voxel::worldToChunk(worldX, 0, worldZ);
    int localX = 0;
    int localZ = 0;
    int localY = 0;
    Voxel::worldToLocal(worldX, 0, worldZ, localX, localY, localZ);

    int minChunkY = Voxel::worldToChunk(0, minY, 0).y;
    int maxChunkY = Voxel::worldToChunk(0, maxY, 0).y;

    for (int cy = maxChunkY; cy >= minChunkY; --cy) {
        Voxel::ChunkCoord coord{baseCoord.x, cy, baseCoord.z};
        Voxel::ChunkBuffer buffer;
        generator.generate(coord, buffer, nullptr);

        for (int ly = Voxel::Chunk::SIZE - 1; ly >= 0; --ly) {
            int worldY = cy * Voxel::Chunk::SIZE + ly;
            if (worldY > maxY || worldY < minY) {
                continue;
            }
            if (!buffer.at(localX, ly, localZ).isAir()) {
                int airY = worldY + 1;
                if (airY <= maxY) {
                    return airY;
                }
                return maxY + 1;
            }
        }
    }

    return maxY;
}

} // namespace Rigel::Voxel
