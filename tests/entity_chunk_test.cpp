#include "TestFramework.h"

#include "Rigel/Entity/EntityChunk.h"
#include "Rigel/Entity/EntityRegion.h"

#include <limits>

using namespace Rigel::Entity;

TEST_CASE(EntityChunk_AddRemove) {
    Entity entity("rigel:test_entity");
    Rigel::Voxel::ChunkCoord coord{0, 0, 0};
    EntityChunk chunk(coord);

    CHECK(!chunk.contains(&entity));
    chunk.addEntity(&entity);
    CHECK(chunk.contains(&entity));
    CHECK_EQ(entity.currentChunk(), &chunk);

    chunk.removeEntity(&entity);
    CHECK(!chunk.contains(&entity));
    CHECK_EQ(entity.currentChunk(), nullptr);
}

TEST_CASE(EntityRegion_MinimumChunkCoordinate) {
    constexpr int minimum = std::numeric_limits<int>::min();
    const EntityRegionCoord region = chunkToRegion({minimum, minimum + 1, 0});
    CHECK_EQ(region.x, minimum / EntityRegionChunkSpan);
    CHECK_EQ(region.y, minimum / EntityRegionChunkSpan);
}
