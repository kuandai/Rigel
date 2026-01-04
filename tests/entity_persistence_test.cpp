#include "TestFramework.h"

#include "Rigel/Entity/EntityPersistence.h"

using namespace Rigel::Entity;
using Rigel::Voxel::ChunkCoord;

TEST_CASE(EntityPersistence_RoundTrip) {
    constexpr float kEpsilon = 1.0e-5f;

    EntityPersistedEntity a;
    a.typeId = "rigel:test_entity";
    a.id = EntityId{123u, 456u, 789u};
    a.position = glm::vec3(1.0f, 2.0f, 3.0f);
    a.velocity = glm::vec3(-1.0f, 0.5f, 4.0f);
    a.viewDirection = glm::vec3(0.0f, 0.0f, -1.0f);
    a.modelId = "entity_models/demo_cube";

    EntityPersistedEntity b;
    b.typeId = "rigel:other_entity";
    b.id = EntityId{5u, 6u, 7u};
    b.position = glm::vec3(-3.5f, 2.25f, 9.0f);
    b.velocity = glm::vec3(0.0f, 0.0f, 0.0f);
    b.viewDirection = glm::vec3(0.0f, 1.0f, 0.0f);

    EntityPersistedChunk chunk1;
    chunk1.coord = ChunkCoord{1, 2, 3};
    chunk1.entities.push_back(a);

    EntityPersistedChunk chunk2;
    chunk2.coord = ChunkCoord{-4, 0, 7};
    chunk2.entities.push_back(b);

    std::vector<EntityPersistedChunk> chunks{chunk1, chunk2};
    std::vector<uint8_t> payload = encodeEntityRegionPayload(chunks);

    std::vector<EntityPersistedChunk> decoded;
    CHECK(decodeEntityRegionPayload(payload, decoded));
    CHECK(decoded.size() == chunks.size());

    CHECK(decoded[0].coord == chunks[0].coord);
    CHECK(decoded[0].entities.size() == 1u);
    CHECK(decoded[0].entities[0].typeId == a.typeId);
    CHECK(decoded[0].entities[0].id == a.id);
    CHECK_NEAR(decoded[0].entities[0].position.x, a.position.x, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].position.y, a.position.y, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].position.z, a.position.z, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].velocity.x, a.velocity.x, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].velocity.y, a.velocity.y, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].velocity.z, a.velocity.z, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].viewDirection.x, a.viewDirection.x, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].viewDirection.y, a.viewDirection.y, kEpsilon);
    CHECK_NEAR(decoded[0].entities[0].viewDirection.z, a.viewDirection.z, kEpsilon);
    CHECK(decoded[0].entities[0].modelId == a.modelId);

    CHECK(decoded[1].coord == chunks[1].coord);
    CHECK(decoded[1].entities.size() == 1u);
    CHECK(decoded[1].entities[0].typeId == b.typeId);
    CHECK(decoded[1].entities[0].id == b.id);
    CHECK_NEAR(decoded[1].entities[0].position.x, b.position.x, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].position.y, b.position.y, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].position.z, b.position.z, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].velocity.x, b.velocity.x, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].velocity.y, b.velocity.y, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].velocity.z, b.velocity.z, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].viewDirection.x, b.viewDirection.x, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].viewDirection.y, b.viewDirection.y, kEpsilon);
    CHECK_NEAR(decoded[1].entities[0].viewDirection.z, b.viewDirection.z, kEpsilon);
    CHECK(decoded[1].entities[0].modelId == b.modelId);
}
