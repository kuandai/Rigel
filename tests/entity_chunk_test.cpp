#include "TestFramework.h"

#include "Rigel/Entity/EntityChunk.h"

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
}
