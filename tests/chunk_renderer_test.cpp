#include "TestFramework.h"

#include "Rigel/Voxel/ChunkRenderer.h"

#include <glm/glm.hpp>

using namespace Rigel::Voxel;

TEST_CASE(ChunkRenderer_MeshTracking) {
    ChunkRenderer renderer;
    CHECK_EQ(renderer.meshCount(), static_cast<size_t>(0));

    ChunkMesh mesh;
    renderer.setChunkMesh({0, 0, 0}, std::move(mesh));
    CHECK(renderer.hasChunkMesh({0, 0, 0}));
    CHECK_EQ(renderer.meshCount(), static_cast<size_t>(1));

    renderer.removeChunkMesh({0, 0, 0});
    CHECK_EQ(renderer.meshCount(), static_cast<size_t>(0));
}

TEST_CASE(ChunkRenderer_SunDirectionNormalized) {
    ChunkRenderer renderer;
    renderer.setSunDirection(glm::vec3(10.0f, 0.0f, 0.0f));

    float length = glm::length(renderer.config().sunDirection);
    CHECK_NEAR(length, 1.0f, 0.0001f);
}
