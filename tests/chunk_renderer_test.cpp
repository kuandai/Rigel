#include "TestFramework.h"

#include "Rigel/Voxel/WorldMeshStore.h"

using namespace Rigel::Voxel;

TEST_CASE(WorldMeshStore_RevisionTracking) {
    WorldMeshStore store;
    ChunkMesh mesh;
    mesh.vertices.resize(3);
    mesh.indices.resize(3);

    store.set({0, 0, 0}, mesh);
    MeshRevision firstRevision{};
    store.forEach([&](const WorldMeshEntry& entry) {
        firstRevision = entry.revision;
    });
    CHECK_EQ(firstRevision.value, static_cast<uint32_t>(1));

    store.set({0, 0, 0}, mesh);
    MeshRevision secondRevision{};
    store.forEach([&](const WorldMeshEntry& entry) {
        secondRevision = entry.revision;
    });
    CHECK_EQ(secondRevision.value, static_cast<uint32_t>(2));

    CHECK(store.contains({0, 0, 0}));
    store.remove({0, 0, 0});
    CHECK(!store.contains({0, 0, 0}));
}

TEST_CASE(WorldMeshStore_VersionIncrement) {
    WorldMeshStore store;
    uint64_t version0 = store.version();

    ChunkMesh mesh;
    mesh.vertices.resize(3);
    mesh.indices.resize(3);
    store.set({1, 0, 0}, mesh);
    CHECK(store.version() != version0);

    uint64_t version1 = store.version();
    store.remove({1, 0, 0});
    CHECK(store.version() != version1);
}
