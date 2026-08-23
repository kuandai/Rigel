#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Voxel/ChunkRenderer.h"
#include "Rigel/Voxel/WorldMeshStore.h"

#include <memory>

using namespace Rigel::Voxel;
namespace Asset = Rigel::Asset;

namespace {

ChunkMesh makeMesh(size_t vertexCount, size_t indexCount) {
    ChunkMesh mesh;
    mesh.vertices.resize(vertexCount);
    mesh.indices.resize(indexCount);
    mesh.layers[static_cast<size_t>(RenderLayer::Opaque)].indexCount =
        static_cast<uint32_t>(indexCount);
    return mesh;
}

Asset::Handle<Asset::ShaderAsset> makeShader() {
    const Asset::ShaderSource source{
        "#version 410 core\n"
        "void main() {\n"
        "    gl_Position = vec4(0.0);\n"
        "}\n",
        "#version 410 core\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    color = vec4(1.0);\n"
        "}\n"
    };
    auto shader = std::make_shared<Asset::ShaderAsset>();
    shader->program = Asset::ShaderCompiler::compile(source, "chunk_renderer_test");
    return Asset::Handle<Asset::ShaderAsset>(std::move(shader), "chunk_renderer_test");
}

GLint boundArrayBufferSize() {
    GLint size = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    return size;
}

} // namespace

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

TEST_CASE(WorldMeshStore_ReinsertAdvancesRevision) {
    WorldMeshStore store;
    const ChunkCoord coord{2, 0, 0};

    store.set(coord, makeMesh(3, 3));
    MeshRevision publishedRevision{};
    store.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            publishedRevision = entry.revision;
        }
    });

    store.remove(coord);
    store.set(coord, makeMesh(6, 6));
    MeshRevision replacementRevision{};
    store.forEach([&](const WorldMeshEntry& entry) {
        if (entry.coord == coord) {
            replacementRevision = entry.revision;
        }
    });

    CHECK(replacementRevision.value > publishedRevision.value);
}

TEST_CASE(ChunkRenderer_ReinsertUploadsWhenRemovalWasNotRendered) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    WorldMeshStore store;
    const ChunkCoord coord{0, 0, 0};
    store.set(coord, makeMesh(3, 3));

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.shader = makeShader();

    ChunkRenderer renderer;
    renderer.render(renderContext);
    CHECK_EQ(
        boundArrayBufferSize(),
        static_cast<GLint>(3 * sizeof(VoxelVertex)));

    store.remove(coord);
    store.set(coord, makeMesh(6, 6));
    renderer.render(renderContext);

    CHECK_EQ(renderer.cachedMeshCount(), static_cast<size_t>(1));
    CHECK_EQ(
        boundArrayBufferSize(),
        static_cast<GLint>(6 * sizeof(VoxelVertex)));
}

TEST_CASE(ChunkRenderer_EmptyInstalledMeshReleasesCachedGeometry) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    WorldMeshStore store;
    const ChunkCoord coord{1, 0, 0};
    store.set(coord, makeMesh(3, 3));

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.shader = makeShader();

    ChunkRenderer renderer;
    renderer.render(renderContext);
    CHECK_EQ(renderer.cachedMeshCount(), static_cast<size_t>(1));

    store.set(coord, ChunkMesh{});
    CHECK(store.contains(coord));
    renderer.render(renderContext);
    CHECK_EQ(renderer.cachedMeshCount(), static_cast<size_t>(0));
}

TEST_CASE(ChunkRenderer_VisibilityTraceRequiresRealNonemptyDraw) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    const ChunkCoord coord{0, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 2});
    const ChunkVisibilityTraceIdentity drawnIdentity{
        coord, 1, 2, 3, 4
    };
    tracer->begin(drawnIdentity);
    tracer->complete(
        drawnIdentity,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    WorldMeshStore store;
    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{drawnIdentity, tracer});
    CHECK(!tracer->snapshot()
               .front()
               .stage(ChunkVisibilityStage::FirstDraw)
               .has_value());

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.shader = makeShader();
    ChunkRenderer renderer;
    renderer.render(renderContext);

    auto records = tracer->snapshot();
    CHECK(records.front().stage(ChunkVisibilityStage::FirstDraw).has_value());

    const ChunkVisibilityTraceIdentity emptyIdentity{
        coord, 2, 2, 3, 5
    };
    tracer->begin(emptyIdentity);
    tracer->complete(
        emptyIdentity,
        ChunkVisibilityOutcome::AcceptedEmptyGeometry);
    store.set(
        coord,
        ChunkMesh{},
        ChunkVisibilityTraceLink{emptyIdentity, tracer});
    renderer.render(renderContext);

    records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(2));
    CHECK(!records.back().stage(ChunkVisibilityStage::FirstDraw).has_value());
}

TEST_CASE(ChunkRenderer_RemovedTraceIsNotDrawnByReplacementMesh) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    const ChunkCoord coord{0, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    const ChunkVisibilityTraceIdentity removedIdentity{
        coord, 1, 2, 3, 4
    };
    tracer->begin(removedIdentity);
    tracer->complete(
        removedIdentity,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    WorldMeshStore store;
    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{removedIdentity, tracer});
    store.remove(coord);
    store.set(coord, makeMesh(6, 6));

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.shader = makeShader();
    ChunkRenderer renderer;
    renderer.render(renderContext);

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(!records.front()
               .stage(ChunkVisibilityStage::FirstDraw)
               .has_value());
}
