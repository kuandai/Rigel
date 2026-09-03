#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/input/GameplayInput.h"
#include "Rigel/input/InputBindingsLoader.h"
#include "Rigel/Render/ChunkDebugPresentation.h"
#include "Rigel/Render/DebugOverlay.h"
#include "Rigel/Render/FrameRenderer.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockTargeting.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
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

TEST_CASE(DebugOverlay_TargetOutlineRendersWithoutDiagnosticsToggle) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();
    Rigel::Asset::AssetManager assets;
    assets.loadManifest("manifest.yaml");
    Rigel::Render::DebugState debug;
    Rigel::Render::initEntityDebug(debug, assets);
    CHECK(!debug.overlayEnabled);
    CHECK(debug.entityDebug.initialized);

    Rigel::Voxel::BlockRegistry registry;
    Rigel::Voxel::BlockType type;
    const std::string identifier = "invented:outline_cube";
    type.identifier = identifier;
    const Rigel::Voxel::BlockID id =
        registry.registerBlock(identifier, std::move(type));
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
    CHECK(glIsEnabled(GL_DEPTH_TEST));
    CHECK(glIsEnabled(GL_CULL_FACE));
    CHECK(!glIsEnabled(GL_BLEND));
    GLboolean depthMask = GL_FALSE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    CHECK_EQ(depthMask, static_cast<GLboolean>(GL_TRUE));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    Rigel::Render::releaseDebugResources(debug);
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
