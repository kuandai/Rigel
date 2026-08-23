#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Render/ChunkDebugPresentation.h"
#include "Rigel/Render/FrameRenderer.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string_view>

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
