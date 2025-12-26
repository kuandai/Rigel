#include "TestFramework.h"

#include "Rigel/Voxel/ChunkMesh.h"

using namespace Rigel::Voxel;

TEST_CASE(ChunkMesh_Counts) {
    ChunkMesh mesh;
    mesh.vertices.resize(6);
    mesh.indices.resize(6);

    CHECK_EQ(mesh.vertexCount(), static_cast<size_t>(6));
    CHECK_EQ(mesh.indexCount(), static_cast<size_t>(6));
    CHECK_EQ(mesh.triangleCount(), static_cast<size_t>(2));
}

TEST_CASE(ChunkMesh_ClearCPUData) {
    ChunkMesh mesh;
    mesh.vertices.resize(4);
    mesh.indices.resize(3);

    mesh.clearCPUData();
    CHECK(mesh.vertices.empty());
    CHECK(mesh.indices.empty());
}
