#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/Entity.h"
#include "Rigel/input/GameplayInput.h"
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/Render/ChunkDebugPresentation.h"
#include "Rigel/Render/DebugOverlay.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldResources.h"
#include "Rigel/Voxel/WorldView.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct DeleteCalls {
    GLsizei generatedVertexArrays = 0;
    GLsizei generatedBuffers = 0;
    GLsizei vertexArrays = 0;
    GLsizei buffers = 0;
    GLuint nextHandle = 1;
};

DeleteCalls* g_deleteCalls = nullptr;

void generateObjects(GLsizei count, GLuint* objects) {
    for (GLsizei i = 0; i < count; ++i) {
        objects[i] = g_deleteCalls->nextHandle++;
    }
}

void GLAPIENTRY generateVertexArrays(GLsizei count, GLuint* objects) {
    g_deleteCalls->generatedVertexArrays += count;
    generateObjects(count, objects);
}

void GLAPIENTRY generateBuffers(GLsizei count, GLuint* objects) {
    g_deleteCalls->generatedBuffers += count;
    generateObjects(count, objects);
}

void GLAPIENTRY countDeleteVertexArrays(GLsizei count, const GLuint*) {
    g_deleteCalls->vertexArrays += count;
}

void GLAPIENTRY countDeleteBuffers(GLsizei count, const GLuint*) {
    g_deleteCalls->buffers += count;
}

void GLAPIENTRY bindVertexArray(GLuint) {
}

GLint GLAPIENTRY failUniformLookup(GLuint, const GLchar*) {
    throw std::runtime_error("uniform lookup failed");
}

class ShaderLoader final : public Rigel::Asset::IAssetLoader {
public:
    std::string_view category() const override {
        return "shaders";
    }

    std::shared_ptr<Rigel::Asset::AssetBase> load(
        const Rigel::Asset::LoadContext&
    ) override {
        return std::make_shared<Rigel::Asset::ShaderAsset>();
    }
};

class ScopedDebugObjectApi {
public:
    explicit ScopedDebugObjectApi(DeleteCalls& calls)
        : m_previousGenVertexArrays(__glewGenVertexArrays)
        , m_previousGenBuffers(__glewGenBuffers)
        , m_previousDeleteVertexArrays(__glewDeleteVertexArrays)
        , m_previousDeleteBuffers(__glewDeleteBuffers)
        , m_previousBindVertexArray(__glewBindVertexArray)
        , m_previousGetUniformLocation(__glewGetUniformLocation) {
        g_deleteCalls = &calls;
        __glewGenVertexArrays = &generateVertexArrays;
        __glewGenBuffers = &generateBuffers;
        __glewDeleteVertexArrays = &countDeleteVertexArrays;
        __glewDeleteBuffers = &countDeleteBuffers;
        __glewBindVertexArray = &bindVertexArray;
        __glewGetUniformLocation = &failUniformLookup;
    }

    ~ScopedDebugObjectApi() {
        __glewGenVertexArrays = m_previousGenVertexArrays;
        __glewGenBuffers = m_previousGenBuffers;
        __glewDeleteVertexArrays = m_previousDeleteVertexArrays;
        __glewDeleteBuffers = m_previousDeleteBuffers;
        __glewBindVertexArray = m_previousBindVertexArray;
        __glewGetUniformLocation = m_previousGetUniformLocation;
        g_deleteCalls = nullptr;
    }

    ScopedDebugObjectApi(const ScopedDebugObjectApi&) = delete;
    ScopedDebugObjectApi& operator=(const ScopedDebugObjectApi&) = delete;

private:
    PFNGLGENVERTEXARRAYSPROC m_previousGenVertexArrays;
    PFNGLGENBUFFERSPROC m_previousGenBuffers;
    PFNGLDELETEVERTEXARRAYSPROC m_previousDeleteVertexArrays;
    PFNGLDELETEBUFFERSPROC m_previousDeleteBuffers;
    PFNGLBINDVERTEXARRAYPROC m_previousBindVertexArray;
    PFNGLGETUNIFORMLOCATIONPROC m_previousGetUniformLocation;
};

Rigel::Voxel::BlockModelCuboid outlineCuboid(
    Rigel::Voxel::BlockModelBounds bounds,
    std::initializer_list<Rigel::Voxel::Direction> faces
) {
    Rigel::Voxel::BlockModelCuboid cuboid;
    cuboid.bounds = bounds;
    for (const Rigel::Voxel::Direction direction : faces) {
        cuboid.faces[static_cast<size_t>(direction)] =
            Rigel::Voxel::BlockModelFace{.textureSlot = "invented"};
    }
    return cuboid;
}

Rigel::Voxel::BlockID registerOutlineBlock(
    Rigel::Voxel::BlockRegistry& registry,
    const std::string& identifier,
    std::vector<Rigel::Voxel::BlockModelCuboid> cuboids,
    Rigel::Voxel::BlockModelOrientation orientation =
        Rigel::Voxel::BlockModelOrientation::Identity
) {
    Rigel::Voxel::BlockType type;
    type.identifier = identifier;
    type.model = Rigel::Voxel::BlockModelInstance(
        std::make_shared<const Rigel::Voxel::BlockModel>(
            identifier + "_model",
            std::vector<std::string>{"invented"},
            std::move(cuboids)));
    type.model.orientation = orientation;
    return registry.registerBlock(identifier, std::move(type));
}

size_t darkPixelCount(int width, int height) {
    std::vector<unsigned char> pixels(
        static_cast<size_t>(width * height * 4));
    glReadPixels(
        0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t count = 0;
    for (size_t index = 0; index < pixels.size(); index += 4) {
        if (pixels[index] < 32) {
            ++count;
        }
    }
    return count;
}

size_t nonBlackPixelCount(int width, int height) {
    std::vector<unsigned char> pixels(
        static_cast<size_t>(width * height * 4));
    glReadPixels(
        0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t count = 0;
    for (size_t index = 0; index < pixels.size(); index += 4) {
        if (pixels[index] != 0 || pixels[index + 1] != 0 ||
            pixels[index + 2] != 0) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE(DebugOverlay_ProductionF1ReleaseTogglesStartupDefault) {
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    assets.registerLoader(
        "input", std::make_unique<Rigel::Input::InputBindingsLoader>());

    const auto shippedBindings =
        Rigel::Input::loadPlayerDefaultBindings(assets);
    CHECK(!shippedBindings->hasAction("debug_overlay"));

    Rigel::Render::FrameRenderer renderer;
    CHECK(!renderer.debugOverlayEnabled());

    Rigel::Input::DebugOverlayListener listener;
    listener.enabled = &renderer.debugOverlayEnabled();

    Rigel::Input::InputState input;
    input.setBindings(Rigel::Input::compileInputBindings(
        *shippedBindings, Rigel::Preferences::InputPreferences{}));
    input.beginFrame();
    input.addListener(&listener);

    input.handleKeyEvent(GLFW_KEY_F1, GLFW_PRESS);
    input.beginFrame();
    CHECK(!renderer.debugOverlayEnabled());
    input.handleKeyEvent(GLFW_KEY_F1, GLFW_RELEASE);
    input.beginFrame();
    CHECK(renderer.debugOverlayEnabled());

    input.handleKeyEvent(GLFW_KEY_F1, GLFW_PRESS);
    input.beginFrame();
    CHECK(renderer.debugOverlayEnabled());
    input.handleKeyEvent(GLFW_KEY_F1, GLFW_RELEASE);
    input.beginFrame();
    CHECK(!renderer.debugOverlayEnabled());
}

TEST_CASE(DebugOverlay_EveryStateMapsToCheckedPresentationStorage) {
    using Rigel::Render::chunkDebugPresentationIndex;
    using Rigel::Render::kChunkDebugPresentationCount;
    using Rigel::Render::kChunkDebugPresentations;
    using DebugState = Rigel::Voxel::ChunkStreamer::DebugState;

    std::array<bool, kChunkDebugPresentationCount> seen{};
    for (size_t value = 0;
         value < static_cast<size_t>(DebugState::Count);
         ++value) {
        const DebugState state = static_cast<DebugState>(value);
        const auto index = chunkDebugPresentationIndex(state);
        CHECK(index.has_value());
        if (!index) {
            continue;
        }
        CHECK(*index < seen.size());
        CHECK(!seen[*index]);
        seen[*index] = true;
        CHECK_EQ(kChunkDebugPresentations[*index].state, state);
        CHECK(!kChunkDebugPresentations[*index].legend.empty());
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) {
        return value;
    }));
    CHECK(!chunkDebugPresentationIndex(DebugState::Count).has_value());
    CHECK(Rigel::Render::kChunkDebugLegendQualification.find(
              "not necessarily drawn") != std::string_view::npos);
}

TEST_CASE(DebugOverlay_DetailPrefersTraceAndKeepsEvidenceSeparate) {
    using ChunkStreamer = Rigel::Voxel::ChunkStreamer;
    using Rigel::Render::selectChunkDebugDetail;

    ChunkStreamer::DebugChunkState nearest;
    nearest.coord = {0, 0, 0};
    nearest.state = ChunkStreamer::DebugState::AcceptedNonemptyGeometry;
    nearest.pipelineOwner = ChunkStreamer::DebugPipelineOwner::Complete;
    nearest.voxelOccupancy =
        ChunkStreamer::DebugVoxelOccupancy::Nonempty;
    nearest.installedGeometry =
        ChunkStreamer::DebugInstalledGeometry::Nonempty;
    nearest.drawEvidence = ChunkStreamer::DebugDrawEvidence::Drawn;

    ChunkStreamer::DebugChunkState traced;
    traced.coord = {3, 2, 1};
    traced.state = ChunkStreamer::DebugState::TerminalFailure;
    traced.pipelineOwner =
        ChunkStreamer::DebugPipelineOwner::TerminalFailure;
    traced.voxelOccupancy =
        ChunkStreamer::DebugVoxelOccupancy::Nonempty;
    traced.installedGeometry =
        ChunkStreamer::DebugInstalledGeometry::Nonempty;
    traced.remeshIntent = ChunkStreamer::DebugRemeshIntent::Pending;
    traced.failure = ChunkStreamer::DebugFailure::Mesh;
    traced.historicalTraceKey =
        Rigel::Voxel::ChunkVisibilityLifecycleKey{traced.coord, 17};
    traced.historicalTraceKind =
        Rigel::Voxel::ChunkVisibilityLifecycleKind::Remesh;
    traced.historicalTraceOutcome =
        Rigel::Voxel::ChunkVisibilityOutcome::Failed;
    traced.historicalTraceDrawOutcome =
        Rigel::Voxel::ChunkVisibilityDrawOutcome::MeshReplacedBeforeDraw;
    traced.drawEvidence = ChunkStreamer::DebugDrawEvidence::NotDrawn;

    const std::array states{nearest, traced};
    const auto detail = selectChunkDebugDetail(states, {0, 0, 0});
    CHECK(detail.has_value());
    CHECK_EQ(detail->coord, traced.coord);

    const auto valueFor = [&](std::string_view label) {
        const auto it = std::find_if(
            detail->lines.begin(),
            detail->lines.end(),
            [&](const auto& line) { return line.label == label; });
        CHECK(it != detail->lines.end());
        return it != detail->lines.end()
            ? it->value
            : std::string{};
    };
    CHECK_EQ(valueFor("Primary state"), "Terminal pipeline failure");
    CHECK_EQ(valueFor("Pipeline owner"), "terminal_failure");
    CHECK_EQ(valueFor("Voxel occupancy"), "nonempty");
    CHECK_EQ(valueFor("Installed CPU geometry"), "nonempty");
    CHECK_EQ(valueFor("Remesh intent"), "pending");
    CHECK_EQ(valueFor("Failure"), "mesh");
    CHECK_EQ(valueFor("Historical trace key"), "(3, 2, 1) #17");
    CHECK_EQ(valueFor("Historical trace kind"), "remesh");
    CHECK_EQ(valueFor("Historical trace outcome"), "failed");
    CHECK_EQ(valueFor("Historical trace draw outcome"),
             "mesh_replaced_before_draw");
    CHECK_EQ(valueFor("Main-pass draw evidence"), "not_drawn");

    CHECK(!selectChunkDebugDetail(
        std::span<const ChunkStreamer::DebugChunkState>{},
        {0, 0, 0}).has_value());
}

TEST_CASE(DebugOverlay_AabbEdgesContainTwelveUniqueNonDiagonalEdges) {
    const glm::vec3 minimum{-2.0f, 3.0f, 5.0f};
    const glm::vec3 maximum{7.0f, 11.0f, 13.0f};
    const auto vertices =
        Rigel::Render::makeAabbEdgeVertices(minimum, maximum);
    std::set<std::pair<int, int>> uniqueEdges;

    const auto cornerIndex = [&](const glm::vec3& vertex) {
        int result = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            CHECK(vertex[axis] == minimum[axis] ||
                  vertex[axis] == maximum[axis]);
            if (vertex[axis] == maximum[axis]) {
                result |= 1 << axis;
            }
        }
        return result;
    };
    for (size_t index = 0; index < vertices.size(); index += 2) {
        const int first = cornerIndex(vertices[index]);
        const int second = cornerIndex(vertices[index + 1]);
        CHECK_EQ(std::popcount(
            static_cast<unsigned>(first ^ second)), 1);
        CHECK(uniqueEdges.emplace(
            std::min(first, second), std::max(first, second)).second);
    }
    CHECK_EQ(uniqueEdges.size(), static_cast<size_t>(12));
}

TEST_CASE(DebugOverlay_AabbEdgePresentationHandlesBoxesAndTransforms) {
    using Rigel::Entity::Aabb;
    using Rigel::Render::buildAabbEdgeLinePresentation;
    using Rigel::Render::makeAabbEdgeVertices;

    const Aabb full{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    const Aabb partial{{0.25f, 0.1f, 0.4f}, {0.75f, 0.6f, 0.9f}};
    const std::array boxes{full, partial};
    const auto presentation = buildAabbEdgeLinePresentation(boxes);
    CHECK_EQ(presentation.size(), static_cast<size_t>(48));

    const auto fullEdges = makeAabbEdgeVertices(full);
    const auto partialEdges = makeAabbEdgeVertices(partial);
    CHECK(std::equal(
        fullEdges.begin(), fullEdges.end(), presentation.begin()));
    CHECK(std::equal(
        partialEdges.begin(),
        partialEdges.end(),
        presentation.begin() + static_cast<std::ptrdiff_t>(fullEdges.size())));

    const Aabb zeroThickness{
        {0.5f, 0.0f, 0.25f},
        {0.5f, 1.0f, 0.25f}};
    const std::array flatBox{zeroThickness};
    const glm::vec3 translation{3.0f, -2.0f, 5.0f};
    constexpr float expansion = 0.125f;
    const auto transformed = buildAabbEdgeLinePresentation(
        flatBox, translation, expansion);
    const auto expected = makeAabbEdgeVertices(
        Aabb{
            {3.375f, -2.125f, 5.125f},
            {3.625f, -0.875f, 5.375f}});
    CHECK_EQ(transformed.size(), expected.size());
    CHECK(std::equal(
        expected.begin(), expected.end(), transformed.begin()));
}

TEST_CASE(DebugOverlay_BlockTargetPresentationUsesEveryOrientedModelCuboid) {
    using namespace Rigel::Voxel;
    using Rigel::Entity::Aabb;
    using Rigel::Render::buildBlockTargetOutlinePresentation;
    using Rigel::Render::makeAabbEdgeVertices;

    BlockRegistry registry;
    BlockType cubeType;
    const std::string cubeIdentifier = "invented:outline_cube";
    cubeType.identifier = cubeIdentifier;
    cubeType.model.orientation = BlockModelOrientation::RotateZ90;
    const BlockID cube = registry.registerBlock(
        cubeIdentifier, std::move(cubeType));

    const BlockID shaped = registerOutlineBlock(
        registry,
        "invented:outline_shape",
        {
            outlineCuboid(
                {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
                {Direction::PosX, Direction::NegX,
                 Direction::PosY, Direction::NegY,
                 Direction::PosZ, Direction::NegZ}),
            outlineCuboid(
                {{0.0f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},
                {Direction::PosX, Direction::PosY}),
            outlineCuboid(
                {{-0.25f, 0.25f, 0.4f}, {0.25f, 0.75f, 0.4f}},
                {Direction::PosZ, Direction::NegZ}),
        },
        BlockModelOrientation::RotateY90);

    BlockTarget target{
        .block = {4, -2, 7},
        .state = BlockState{shaped},
        .cuboidIndex = 1,
    };
    const std::vector<glm::vec3> shapedPresentation =
        buildBlockTargetOutlinePresentation(registry, &target);
    CHECK_EQ(shapedPresentation.size(), static_cast<size_t>(72));

    constexpr float expansion = 0.002f;
    const std::array expectedBounds = {
        Aabb{{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
        Aabb{{0.0f, 0.5f, 0.0f}, {0.5f, 1.0f, 1.0f}},
        Aabb{{0.6f, 0.25f, -0.25f}, {0.6f, 0.75f, 0.25f}},
    };
    for (size_t index = 0; index < expectedBounds.size(); ++index) {
        const auto expected = makeAabbEdgeVertices(
            expectedBounds[index], glm::vec3(target.block), expansion);
        CHECK(std::equal(
            expected.begin(),
            expected.end(),
            shapedPresentation.begin() + static_cast<std::ptrdiff_t>(
                index * expected.size())));
    }

    target.cuboidIndex = 2;
    CHECK_EQ(
        buildBlockTargetOutlinePresentation(registry, &target),
        shapedPresentation);

    target.block = {-3, 5, 2};
    target.state = BlockState{cube};
    target.cuboidIndex = 0;
    const std::vector<glm::vec3> cubePresentation =
        buildBlockTargetOutlinePresentation(registry, &target);
    const auto expectedCube = makeAabbEdgeVertices(
        Aabb{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        glm::vec3(target.block),
        expansion);
    CHECK_EQ(cubePresentation.size(), expectedCube.size());
    CHECK(std::equal(
        expectedCube.begin(), expectedCube.end(), cubePresentation.begin()));

    target.state = BlockState{BlockRegistry::airId()};
    CHECK(buildBlockTargetOutlinePresentation(registry, &target).empty());
    CHECK(buildBlockTargetOutlinePresentation(registry, nullptr).empty());
    target.state.id.type = std::numeric_limits<uint16_t>::max();
    CHECK(buildBlockTargetOutlinePresentation(registry, &target).empty());
}

TEST_CASE(DebugOverlay_TargetOutlineRendersWithoutDiagnosticsToggle) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    Rigel::Render::DebugState debug;
    Rigel::Render::initEntityDebug(debug, assets);
    CHECK(!debug.overlayEnabled);
    CHECK(debug.entityDebug.initialized);

    const auto callerShader =
        assets.get<Rigel::Asset::ShaderAsset>("shaders/frame_graph");
    GLuint callerVao = 0;
    GLuint callerBuffer = 0;
    glGenVertexArrays(1, &callerVao);
    glGenBuffers(1, &callerBuffer);
    glBindVertexArray(callerVao);
    glBindBuffer(GL_ARRAY_BUFFER, callerBuffer);
    callerShader->bind();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);

    Rigel::Voxel::BlockRegistry registry;
    const std::string identifier = "invented:outline_cube";
    const Rigel::Voxel::BlockID id = registerOutlineBlock(
        registry,
        identifier,
        {
            outlineCuboid(
                {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}},
                {Rigel::Voxel::Direction::PosX,
                 Rigel::Voxel::Direction::NegX}),
            outlineCuboid(
                {{0.25f, 0.5f, 0.25f}, {0.75f, 1.0f, 0.75f}},
                {Rigel::Voxel::Direction::PosY}),
        });
    const Rigel::Voxel::BlockTarget target{
        .block = {0, 0, 0},
        .state = Rigel::Voxel::BlockState{id},
    };

    constexpr int extent = 64;
    glViewport(0, 0, extent, extent);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Rigel::Render::renderBlockTargetOutline(
        debug,
        registry,
        &target,
        glm::mat4(1.0f),
        glm::ortho(-0.5f, 1.5f, -0.5f, 1.5f, -2.0f, 2.0f));

    std::vector<unsigned char> pixels(
        static_cast<size_t>(extent * extent * 4));
    glReadPixels(
        0, 0, extent, extent, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t litPixels = 0;
    for (size_t index = 0; index < pixels.size(); index += 4) {
        if (pixels[index] != 0 || pixels[index + 1] != 0 ||
            pixels[index + 2] != 0) {
            ++litPixels;
        }
    }
    CHECK(litPixels > 0);
    CHECK(!glIsEnabled(GL_DEPTH_TEST));
    CHECK(glIsEnabled(GL_CULL_FACE));
    CHECK(glIsEnabled(GL_BLEND));
    GLboolean depthMask = GL_FALSE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    CHECK_EQ(depthMask, static_cast<GLboolean>(GL_TRUE));
    GLint currentProgram = 0;
    GLint currentVao = 0;
    GLint currentBuffer = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &currentBuffer);
    CHECK_EQ(currentProgram, static_cast<GLint>(callerShader->program));
    CHECK_EQ(currentVao, static_cast<GLint>(callerVao));
    CHECK_EQ(currentBuffer, static_cast<GLint>(callerBuffer));
    glBindBuffer(GL_ARRAY_BUFFER, debug.entityDebug.vbo);
    GLint uploadedBytes = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &uploadedBytes);
    CHECK_EQ(
        uploadedBytes,
        static_cast<GLint>(48 * sizeof(glm::vec3)));
    glBindBuffer(GL_ARRAY_BUFFER, callerBuffer);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Rigel::Render::renderBlockTargetOutline(
        debug,
        registry,
        &target,
        glm::mat4(1.0f),
        glm::ortho(-0.5f, 1.5f, -0.5f, 1.5f, -2.0f, 2.0f));
    CHECK_EQ(nonBlackPixelCount(extent, extent), static_cast<size_t>(0));

    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Rigel::Render::renderBlockTargetOutline(
        debug,
        registry,
        nullptr,
        glm::mat4(1.0f),
        glm::ortho(-0.5f, 1.5f, -0.5f, 1.5f, -2.0f, 2.0f));
    CHECK_EQ(nonBlackPixelCount(extent, extent), static_cast<size_t>(0));

    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDeleteVertexArrays(1, &callerVao);
    glDeleteBuffers(1, &callerBuffer);
    Rigel::Render::releaseDebugResources(debug);
    assets.clearCache();
}

TEST_CASE(DebugOverlay_FrameTargetUsesStableProjectionAcrossTaaModes) {
    using namespace Rigel::Voxel;

    constexpr int extent = 64;
    Rigel::Test::HiddenOpenGLContext context(extent, extent);
    context.require();
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");

    WorldResources resources;
    BlockType type;
    const std::string identifier = "invented:frame_outline";
    type.identifier = identifier;
    const BlockID block = resources.registry().registerBlock(
        identifier, std::move(type));
    World world(resources);
    WorldView view(world, resources);
    view.initialize(assets);

    Rigel::Render::FrameRenderer renderer;
    renderer.initialize(assets);
    constexpr float verticalFovDegrees = 60.0f;
    renderer.setVerticalFovDegrees(verticalFovDegrees);

    const BlockTarget target{
        .block = {0, 0, 0},
        .state = BlockState{block},
    };
    const glm::vec3 cameraPosition{0.5f, 0.5f, 3.0f};
    const glm::vec3 cameraTarget{0.5f, 0.5f, 0.5f};
    const glm::mat4 expectedViewProjection = glm::perspective(
        glm::radians(verticalFovDegrees), 1.0f, 0.1f,
        view.projectionFarPlaneWorldUnits()) * glm::lookAt(
            cameraPosition,
            cameraTarget,
            glm::vec3(0.0f, 1.0f, 0.0f));
    const auto lineShader =
        assets.get<Rigel::Asset::ShaderAsset>("shaders/entity_debug");
    const GLint viewProjectionLocation =
        lineShader->uniform("u_viewProjection");
    CHECK(viewProjectionLocation >= 0);

    for (const bool taaEnabled : {false, true}) {
        RenderProfile profile = view.renderProfile();
        profile.temporalAA.enabled = taaEnabled;
        view.setRenderProfileForDiagnostics(profile);

        renderer.render({
            .world = world,
            .worldView = view,
            .cameraPosition = cameraPosition,
            .cameraTarget = cameraTarget,
            .cameraForward = glm::normalize(cameraTarget - cameraPosition),
            .viewportWidth = extent,
            .viewportHeight = extent,
            .blockTarget = &target,
        });

        std::array<float, 16> actualViewProjection{};
        glGetUniformfv(
            lineShader->program,
            viewProjectionLocation,
            actualViewProjection.data());
        const float* expected = glm::value_ptr(expectedViewProjection);
        for (size_t index = 0;
             index < actualViewProjection.size(); ++index) {
            CHECK_NEAR(
                actualViewProjection[index], expected[index], 0.00001f);
        }
        CHECK(darkPixelCount(extent, extent) > 0);
        CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        renderer.render({
            .world = world,
            .worldView = view,
            .cameraPosition = cameraPosition,
            .cameraTarget = cameraTarget,
            .cameraForward = glm::normalize(cameraTarget - cameraPosition),
            .viewportWidth = extent,
            .viewportHeight = extent,
            .blockTarget = nullptr,
        });
        CHECK_EQ(darkPixelCount(extent, extent), static_cast<size_t>(0));
        CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    renderer.release();
    view.releaseRenderResources();
    resources.releaseRenderResources();
    assets.clearCache();
}

TEST_CASE(DebugOverlay_EntityBoxesRetainSharedResourceLifecycle) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    Rigel::Render::DebugState debug;
    Rigel::Render::initEntityDebug(debug, assets);
    debug.overlayEnabled = true;

    Rigel::Voxel::WorldResources resources;
    Rigel::Voxel::World world(resources);
    auto entity = std::make_unique<Rigel::Entity::Entity>(
        "invented:outlined_entity");
    entity->setLocalBounds({{-0.4f, -0.3f, -0.2f},
                            {0.4f, 0.3f, 0.2f}});
    entity->setPosition(0.0f, 0.0f, 0.0f);
    CHECK(!world.entities().spawn(std::move(entity)).isNull());

    constexpr int extent = 64;
    glViewport(0, 0, extent, extent);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Rigel::Render::renderEntityDebugBoxes(
        debug,
        &world,
        glm::mat4(1.0f),
        glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -2.0f, 2.0f));

    std::vector<unsigned char> pixels(
        static_cast<size_t>(extent * extent * 4));
    glReadPixels(
        0, 0, extent, extent, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const size_t litPixels = static_cast<size_t>(std::count_if(
        pixels.begin(), pixels.end(), [](unsigned char component) {
            return component != 0;
        }));
    CHECK(litPixels > static_cast<size_t>(extent * extent));

    const GLuint lineVao = debug.entityDebug.vao;
    const GLuint lineVbo = debug.entityDebug.vbo;
    CHECK(glIsVertexArray(lineVao));
    CHECK(glIsBuffer(lineVbo));
    Rigel::Render::releaseDebugResources(debug);
    CHECK(!debug.entityDebug.initialized);
    CHECK_EQ(debug.entityDebug.vao, static_cast<GLuint>(0));
    CHECK_EQ(debug.entityDebug.vbo, static_cast<GLuint>(0));
    CHECK(!glIsVertexArray(lineVao));
    CHECK(!glIsBuffer(lineVbo));
    CHECK_NO_THROW(Rigel::Render::releaseDebugResources(debug));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    assets.clearCache();
}

TEST_CASE(DebugOverlay_PartialAcquisitionCleanupRunsOnce) {
    Rigel::Asset::AssetManager assets;
    assets.registerLoader("shaders", std::make_unique<ShaderLoader>());
    assets.loadManifest("manifest.yaml");

    Rigel::Render::FrameRenderer renderer;
    DeleteCalls deleteCalls;
    ScopedDebugObjectApi debugObjectApi(deleteCalls);

    CHECK_THROWS(([&] {
        try {
            renderer.initialize(assets);
        } catch (...) {
            renderer.release();
            renderer.release();
            throw;
        }
    }()));

    CHECK_EQ(deleteCalls.generatedVertexArrays, 1);
    CHECK_EQ(deleteCalls.generatedBuffers,
             static_cast<GLsizei>(
                 Rigel::Render::kChunkDebugPresentationCount));
    CHECK_EQ(deleteCalls.vertexArrays, deleteCalls.generatedVertexArrays);
    CHECK_EQ(deleteCalls.buffers, deleteCalls.generatedBuffers);
}
