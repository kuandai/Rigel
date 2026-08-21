#include "TestFramework.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Render/FrameRenderer.h"

#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

struct DeleteCalls {
    GLsizei vertexArrays = 0;
    GLsizei buffers = 0;
    GLuint nextHandle = 1;
};

DeleteCalls* g_deleteCalls = nullptr;

void GLAPIENTRY generateObjects(GLsizei count, GLuint* objects) {
    for (GLsizei i = 0; i < count; ++i) {
        objects[i] = g_deleteCalls->nextHandle++;
    }
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
        __glewGenVertexArrays = &generateObjects;
        __glewGenBuffers = &generateObjects;
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

    CHECK_EQ(deleteCalls.vertexArrays, 1);
    CHECK_EQ(deleteCalls.buffers, 5);
}
