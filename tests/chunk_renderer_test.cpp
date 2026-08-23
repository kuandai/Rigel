#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Asset/ShaderCompiler.h"
#include "Rigel/Voxel/ChunkRenderer.h"
#include "Rigel/Voxel/WorldMeshStore.h"

#include <array>
#include <memory>
#include <stdexcept>

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

TEST_CASE(WorldMeshStore_TraceOwnershipSurvivesUntilNegativeDrawTransition) {
    WorldMeshStore store;
    const ChunkCoord coord{2, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    const auto key = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    std::weak_ptr<ChunkVisibilityTracer> observer = tracer;

    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{
            key,
            ChunkVisibilityLifecycleKind::CameraDemand,
            tracer});
    tracer.reset();
    CHECK(!observer.expired());

    auto retainedTracer = observer.lock();
    store.remove(coord);
    CHECK(retainedTracer != nullptr);
    retainedTracer->complete(
        key,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    const auto records = retainedTracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().drawOutcome,
        ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
    CHECK(!records.front().stage(ChunkVisibilityStage::FirstDraw).has_value());
}

TEST_CASE(WorldMeshStore_ReplacingMeshTerminatesPriorDrawCorrelation) {
    WorldMeshStore store;
    const ChunkCoord coord{3, 0, 0};
    auto firstTracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    auto secondTracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    const auto firstKey = *firstTracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    const auto secondKey = *secondTracer->begin(
        ChunkVisibilityLifecycleKind::Remesh);

    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{
            firstKey,
            ChunkVisibilityLifecycleKind::CameraDemand,
            firstTracer});
    store.set(
        coord,
        makeMesh(6, 6),
        ChunkVisibilityTraceLink{
            secondKey,
            ChunkVisibilityLifecycleKind::Remesh,
            secondTracer});
    firstTracer->complete(
        firstKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    secondTracer->complete(
        secondKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    CHECK_EQ(
        firstTracer->snapshot().front().drawOutcome,
        ChunkVisibilityDrawOutcome::MeshReplacedBeforeDraw);
    CHECK(!secondTracer->snapshot().front().drawOutcome.has_value());
}

TEST_CASE(WorldMeshStore_CachedTraceRemovalBeforeCompletionIsRecorded) {
    WorldMeshStore store;
    const ChunkCoord coord{4, 0, 0};
    store.set(coord, makeMesh(3, 3));

    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    const auto key = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    CHECK_EQ(
        store.attachCachedVisibilityTrace(
            coord,
            ChunkVisibilityTraceLink{
                key,
                ChunkVisibilityLifecycleKind::CameraDemand,
                tracer}),
        CachedMeshTraceAttachment::NonemptyGeometry);

    store.remove(coord);
    tracer->complete(
        key,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);

    const auto records = tracer->snapshot();
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(
        records.front().outcome,
        ChunkVisibilityOutcome::CachedNonemptyGeometry);
    CHECK_EQ(
        records.front().drawOutcome,
        ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
    CHECK(!records.front().stage(ChunkVisibilityStage::FirstDraw).has_value());
}

TEST_CASE(WorldMeshStore_ReleasesTraceOwnershipAfterDraw) {
    WorldMeshStore store;
    const ChunkCoord coord{4, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    const auto key = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{
            key,
            ChunkVisibilityLifecycleKind::CameraDemand,
            tracer});
    tracer->complete(
        key,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    std::weak_ptr<ChunkVisibilityTracer> observer = tracer;
    tracer->observeDraw(key);
    CHECK_EQ(
        tracer->snapshot().front().drawOutcome,
        ChunkVisibilityDrawOutcome::Drawn);
    tracer.reset();
    CHECK(!observer.expired());
    store.finishVisibilityDraw(key);
    CHECK(observer.expired());
}

TEST_CASE(WorldMeshStore_TraceReplacementConsumesPublishedDrawLink) {
    WorldMeshStore store;
    const ChunkCoord coord{5, 0, 0};
    auto tracer = std::make_shared<ChunkVisibilityTracer>(
        ChunkVisibilityTracer::Config{coord, 1});
    const auto key = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{
            key,
            ChunkVisibilityLifecycleKind::CameraDemand,
            tracer});
    tracer->complete(
        key,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    store.endVisibilityTrace(
        coord,
        tracer,
        ChunkVisibilityDrawOutcome::TraceReplacedBeforeDraw);

    CHECK_EQ(
        tracer->measurement().records.front().drawOutcome,
        ChunkVisibilityDrawOutcome::TraceReplacedBeforeDraw);
    std::weak_ptr<ChunkVisibilityTracer> observer = tracer;
    tracer.reset();
    CHECK(observer.expired());
}

TEST_CASE(WorldMeshStore_ClearContainsThrowingTraceClocks) {
    WorldMeshStore store;
    const std::array coords{
        ChunkCoord{6, 0, 0},
        ChunkCoord{7, 0, 0}
    };
    std::array<std::shared_ptr<ChunkVisibilityTracer>, 2> tracers;
    for (size_t index = 0; index < tracers.size(); ++index) {
        tracers[index] = std::make_shared<ChunkVisibilityTracer>(
            ChunkVisibilityTracer::Config{coords[index], 1},
            []() -> ChunkVisibilityTimePoint {
                throw std::runtime_error("clock failure during clear");
            });
        const auto key = *tracers[index]->begin(
            ChunkVisibilityLifecycleKind::CameraDemand);
        store.set(
            coords[index],
            makeMesh(3, 3),
            ChunkVisibilityTraceLink{
                key,
                ChunkVisibilityLifecycleKind::CameraDemand,
                tracers[index]});
        tracers[index]->complete(
            key,
            ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
    }

    CHECK_NO_THROW(store.clear());
    for (const auto& tracer : tracers) {
        const auto measurement = tracer->measurement();
        CHECK_EQ(
            measurement.records.front().drawOutcome,
            ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
        CHECK_EQ(measurement.stats.clockFailures, static_cast<uint64_t>(2));
    }
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
    const auto drawnKey = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);
    tracer->bindMeshTask(
        drawnKey,
        ChunkVisibilityMeshTaskIdentity{1, 2, 3, 4});

    WorldMeshStore store;
    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{
            drawnKey,
            ChunkVisibilityLifecycleKind::CameraDemand,
            tracer});
    CHECK(!tracer->snapshot()
               .front()
               .stage(ChunkVisibilityStage::FirstDraw)
               .has_value());

    WorldRenderContext renderContext;
    renderContext.meshes = &store;
    renderContext.shader = makeShader();
    ChunkRenderer renderer;
    renderer.render(renderContext);
    tracer->complete(
        drawnKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);

    auto records = tracer->snapshot();
    CHECK(records.front().stage(ChunkVisibilityStage::FirstDraw).has_value());

    const auto emptyKey = *tracer->begin(
        ChunkVisibilityLifecycleKind::Remesh);
    tracer->bindMeshTask(
        emptyKey,
        ChunkVisibilityMeshTaskIdentity{2, 2, 3, 5});
    store.set(
        coord,
        ChunkMesh{},
        ChunkVisibilityTraceLink{
            emptyKey,
            ChunkVisibilityLifecycleKind::Remesh,
            tracer});
    tracer->complete(
        emptyKey,
        ChunkVisibilityOutcome::AcceptedEmptyGeometry);
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
    const auto removedKey = *tracer->begin(
        ChunkVisibilityLifecycleKind::CameraDemand);

    WorldMeshStore store;
    store.set(
        coord,
        makeMesh(3, 3),
        ChunkVisibilityTraceLink{
            removedKey,
            ChunkVisibilityLifecycleKind::CameraDemand,
            tracer});
    store.remove(coord);
    tracer->complete(
        removedKey,
        ChunkVisibilityOutcome::AcceptedNonemptyGeometry);
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
    CHECK_EQ(
        records.front().drawOutcome,
        ChunkVisibilityDrawOutcome::MeshRemovedBeforeDraw);
}
