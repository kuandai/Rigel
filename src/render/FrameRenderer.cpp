#include "Rigel/Render/FrameRenderer.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Core/Profiler.h"
#include "Rigel/Render/CameraProjection.h"
#include "Rigel/Render/DebugOverlay.h"
#include "Rigel/Render/TemporalJitter.h"
#include "FrameRendererTestAccess.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Voxel/WorldView.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <stdexcept>

#include <GL/glew.h>
#include <glm/mat4x4.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

namespace Rigel::Render {

struct FrameRenderer::Impl {
    struct TemporalAA {
        GLuint sceneFbo = 0;
        GLuint sceneColor = 0;
        GLuint sceneDepth = 0;
        GLuint resolveFbo = 0;
        std::array<GLuint, 2> history{};
        std::array<GLuint, 2> historyDepth{};
        GLuint quadVao = 0;
        Asset::Handle<Asset::ShaderAsset> shader;
        GLint locCurrentColor = -1;
        GLint locCurrentDepth = -1;
        GLint locHistory = -1;
        GLint locHistoryDepth = -1;
        GLint locCurrentJitter = -1;
        GLint locInvViewProjection = -1;
        GLint locPrevViewProjection = -1;
        GLint locHistoryBlend = -1;
        GLint locHistoryValid = -1;
        GLint locTexelSize = -1;
        int width = 0;
        int height = 0;
        int historyIndex = 0;
        bool initialized = false;
        bool historyValid = false;
        glm::mat4 prevViewProjection{1.0f};
        TemporalJitterSequence jitter;
    };

    Asset::AssetManager* assets = nullptr;
    DebugState debug;
    TemporalAA taa;
    std::optional<double> verticalFovDegrees;

    std::pair<glm::mat4, glm::mat4> cameraProjections(
        float mainAspect,
        float nearPlane,
        float farPlane) const {
        if (!verticalFovDegrees) {
            throw std::logic_error(
                "FrameRenderer requires an effective vertical FOV before "
                "rendering");
        }
        const float fov = static_cast<float>(*verticalFovDegrees);
        return {
            makeCameraProjection(fov, mainAspect, nearPlane, farPlane),
            makeCameraProjection(fov, 1.0f, nearPlane, farPlane)};
    }

    void clearTarget(GLuint framebuffer, int width, int height) {
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, width, height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void releaseTaaTargets() {
        if (taa.sceneFbo != 0) {
            glDeleteFramebuffers(1, &taa.sceneFbo);
            taa.sceneFbo = 0;
        }
        if (taa.resolveFbo != 0) {
            glDeleteFramebuffers(1, &taa.resolveFbo);
            taa.resolveFbo = 0;
        }
        if (taa.sceneColor != 0) {
            glDeleteTextures(1, &taa.sceneColor);
            taa.sceneColor = 0;
        }
        if (taa.sceneDepth != 0) {
            glDeleteTextures(1, &taa.sceneDepth);
            taa.sceneDepth = 0;
        }
        for (GLuint& texture : taa.history) {
            if (texture != 0) {
                glDeleteTextures(1, &texture);
                texture = 0;
            }
        }
        for (GLuint& texture : taa.historyDepth) {
            if (texture != 0) {
                glDeleteTextures(1, &texture);
                texture = 0;
            }
        }
        taa.width = 0;
        taa.height = 0;
        taa.historyValid = false;
    }

    void initTaa() {
        if (taa.initialized || !assets) {
            return;
        }
        try {
            taa.shader = assets->get<Asset::ShaderAsset>("shaders/taa_resolve");
        } catch (const std::exception& e) {
            spdlog::warn("TAA shader unavailable: {}", e.what());
            return;
        }

        glGenVertexArrays(1, &taa.quadVao);
        glBindVertexArray(taa.quadVao);
        glBindVertexArray(0);

        taa.locCurrentColor = taa.shader->uniform("u_currentColor");
        taa.locCurrentDepth = taa.shader->uniform("u_currentDepth");
        taa.locHistory = taa.shader->uniform("u_historyColor");
        taa.locHistoryDepth = taa.shader->uniform("u_historyDepth");
        taa.locCurrentJitter = taa.shader->uniform("u_currentJitter");
        taa.locInvViewProjection = taa.shader->uniform("u_invViewProjection");
        taa.locPrevViewProjection = taa.shader->uniform("u_prevViewProjection");
        taa.locHistoryBlend = taa.shader->uniform("u_historyBlend");
        taa.locHistoryValid = taa.shader->uniform("u_historyValid");
        taa.locTexelSize = taa.shader->uniform("u_texelSize");

        taa.initialized = true;
    }

    void ensureTaaTargets(int width, int height) {
        initTaa();
        if (!taa.initialized || width <= 0 || height <= 0) {
            return;
        }
        if (taa.width == width && taa.height == height && taa.sceneFbo != 0) {
            return;
        }

        releaseTaaTargets();
        taa.width = width;
        taa.height = height;

        glGenTextures(1, &taa.sceneColor);
        glBindTexture(GL_TEXTURE_2D, taa.sceneColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &taa.sceneDepth);
        glBindTexture(GL_TEXTURE_2D, taa.sceneDepth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        glGenTextures(2, taa.history.data());
        for (GLuint& texture : taa.history) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glGenTextures(2, taa.historyDepth.data());
        for (GLuint& texture : taa.historyDepth) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                         GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }

        glGenFramebuffers(1, &taa.sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, taa.sceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, taa.sceneColor, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, taa.sceneDepth, 0);
        GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::warn("TAA scene FBO incomplete: status=0x{:X}", status);
        }

        glGenFramebuffers(1, &taa.resolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    bool prepareTaa(const Voxel::TemporalAAProfile& profile,
                    int width,
                    int height,
                    glm::mat4& projection,
                    glm::vec2& jitter) {
        if (!profile.enabled) {
            taa.historyValid = false;
            return false;
        }

        ensureTaaTargets(width, height);
        if (!taa.initialized || taa.sceneFbo == 0) {
            return false;
        }

        jitter = taa.jitter.next(width, height, profile.jitterScale);
        projection[2][0] += jitter.x;
        projection[2][1] += jitter.y;
        return true;
    }

    bool resolveTaa(const glm::mat4& invViewProjection,
                    const glm::mat4& viewProjection,
                    const glm::vec2& jitterUv,
                    float blend) {
        if (!taa.initialized || taa.resolveFbo == 0 || taa.sceneColor == 0) {
            return false;
        }

        int readIndex = taa.historyIndex;
        int writeIndex = (taa.historyIndex + 1) % 2;

        glBindFramebuffer(GL_FRAMEBUFFER, taa.resolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, taa.history[writeIndex], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, taa.historyDepth[writeIndex], 0);
        GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);

        taa.shader->bind();
        if (taa.locCurrentColor >= 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, taa.sceneColor);
            glUniform1i(taa.locCurrentColor, 0);
        }
        if (taa.locCurrentDepth >= 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, taa.sceneDepth);
            glUniform1i(taa.locCurrentDepth, 1);
        }
        if (taa.locHistory >= 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, taa.history[readIndex]);
            glUniform1i(taa.locHistory, 2);
        }
        if (taa.locHistoryDepth >= 0) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, taa.historyDepth[readIndex]);
            glUniform1i(taa.locHistoryDepth, 3);
        }
        if (taa.locCurrentJitter >= 0) {
            glUniform2fv(taa.locCurrentJitter, 1, glm::value_ptr(jitterUv));
        }
        if (taa.locInvViewProjection >= 0) {
            glUniformMatrix4fv(taa.locInvViewProjection, 1, GL_FALSE,
                               glm::value_ptr(invViewProjection));
        }
        if (taa.locPrevViewProjection >= 0) {
            glUniformMatrix4fv(taa.locPrevViewProjection, 1, GL_FALSE,
                               glm::value_ptr(taa.prevViewProjection));
        }
        if (taa.locHistoryBlend >= 0) {
            glUniform1f(taa.locHistoryBlend, blend);
        }
        if (taa.locHistoryValid >= 0) {
            glUniform1i(taa.locHistoryValid, taa.historyValid ? 1 : 0);
        }
        if (taa.locTexelSize >= 0) {
            glm::vec2 texelSize(1.0f / static_cast<float>(taa.width),
                                1.0f / static_cast<float>(taa.height));
            glUniform2fv(taa.locTexelSize, 1, glm::value_ptr(texelSize));
        }

        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);

        glBindVertexArray(taa.quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.resolveFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, taa.sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, taa.resolveFbo);
        glBlitFramebuffer(0, 0, taa.width, taa.height,
                          0, 0, taa.width, taa.height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        taa.historyValid = true;
        taa.historyIndex = writeIndex;
        taa.prevViewProjection = viewProjection;
        return true;
    }
};

FrameRenderer::FrameRenderer() : m_impl(std::make_unique<Impl>()) {}

FrameRenderer::~FrameRenderer() = default;

void FrameRenderer::initialize(Asset::AssetManager& assets) {
    m_impl->assets = &assets;
    initDebugField(m_impl->debug, assets);
    initFrameGraph(m_impl->debug, assets);
    initEntityDebug(m_impl->debug, assets);
    m_impl->initTaa();
}

void FrameRenderer::release() {
    releaseDebugResources(m_impl->debug);
    m_impl->debug.field.shader = {};
    m_impl->debug.frameGraph.shader = {};
    m_impl->debug.entityDebug.shader = {};

    if (m_impl->taa.quadVao != 0) {
        glDeleteVertexArrays(1, &m_impl->taa.quadVao);
        m_impl->taa.quadVao = 0;
    }
    m_impl->releaseTaaTargets();
    m_impl->taa.shader = {};
    m_impl->taa.initialized = false;
    m_impl->assets = nullptr;
}

void FrameRenderer::recordFrameTime(float seconds) {
    Render::recordFrameTime(m_impl->debug, seconds);
}

void FrameRenderer::setVerticalFovDegrees(double verticalFovDegrees) {
    if (m_impl->verticalFovDegrees &&
        *m_impl->verticalFovDegrees == verticalFovDegrees) {
        return;
    }
    m_impl->verticalFovDegrees = verticalFovDegrees;
    m_impl->taa.historyValid = false;
    m_impl->taa.jitter.reset();
}

void FrameRenderer::render(const FrameRenderContext& context) {
    const auto& profile = context.worldView.renderProfile();
    float aspect = context.viewportHeight > 0
        ? static_cast<float>(context.viewportWidth) /
            static_cast<float>(context.viewportHeight)
        : 1.0f;
    float nearPlane = 0.1f;
    float farPlane = context.worldView.projectionFarPlaneWorldUnits();

    const auto [mainProjection, debugProjection] =
        m_impl->cameraProjections(aspect, nearPlane, farPlane);
    glm::mat4 projection = mainProjection;
    glm::mat4 projectionNoJitter = projection;
    glm::mat4 view = glm::lookAt(
        context.cameraPosition,
        context.cameraTarget,
        glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec2 jitter(0.0f);
    bool useTaa = m_impl->prepareTaa(
        profile.temporalAA,
        context.viewportWidth,
        context.viewportHeight,
        projection,
        jitter);

    m_impl->clearTarget(
        useTaa ? m_impl->taa.sceneFbo : 0,
        context.viewportWidth,
        context.viewportHeight);

    context.worldView.render(
        view,
        projection,
        context.cameraPosition,
        nearPlane,
        farPlane,
        context.deltaTime);

    if (useTaa) {
        renderEntityDebugBoxes(m_impl->debug, &context.world, view, projection);
        {
            PROFILE_SCOPE("TAA");
            glm::mat4 viewProjectionNoJitter = projectionNoJitter * view;
            m_impl->resolveTaa(
                glm::inverse(viewProjectionNoJitter),
                viewProjectionNoJitter,
                jitter * 0.5f,
                profile.temporalAA.blend);
        }
        glViewport(0, 0, context.viewportWidth, context.viewportHeight);
    } else {
        renderEntityDebugBoxes(
            m_impl->debug, &context.world, view, projectionNoJitter);
    }

    // Selection feedback is composited after the resolved scene. This keeps
    // it out of temporal history and uses the stable projection while still
    // testing against scene depth (resolved depth is blitted above for TAA).
    renderBlockTargetOutline(
        m_impl->debug,
        context.world.blockRegistry(),
        context.blockTarget,
        view,
        projectionNoJitter);

    renderDebugField(
        m_impl->debug,
        &context.worldView,
        context.cameraPosition,
        context.cameraTarget,
        context.cameraForward,
        context.viewportWidth,
        context.viewportHeight,
        debugProjection);
    renderFrameGraph(m_impl->debug);
}

void FrameRenderer::clear(int viewportWidth, int viewportHeight) {
    m_impl->debug.debugStates.clear();
    m_impl->debug.chunkDetail.reset();
    m_impl->clearTarget(0, viewportWidth, viewportHeight);
}

bool& FrameRenderer::debugOverlayEnabled() {
    return m_impl->debug.overlayEnabled;
}

bool& FrameRenderer::profilerWindowEnabled() {
    return m_impl->debug.imguiEnabled;
}

const ChunkDebugDetailPresentation* FrameRenderer::chunkDebugDetail() const {
    return m_impl->debug.chunkDetail
        ? &*m_impl->debug.chunkDetail
        : nullptr;
}

double FrameRendererTestAccess::verticalFovDegrees(
    const FrameRenderer& renderer) {
    return renderer.m_impl->verticalFovDegrees.value();
}

std::pair<glm::mat4, glm::mat4>
FrameRendererTestAccess::cameraProjections(
    const FrameRenderer& renderer,
    float mainAspect,
    float nearPlane,
    float farPlane) {
    return renderer.m_impl->cameraProjections(
        mainAspect, nearPlane, farPlane);
}

bool FrameRendererTestAccess::temporalHistoryValid(
    const FrameRenderer& renderer) {
    return renderer.m_impl->taa.historyValid;
}

void FrameRendererTestAccess::markTemporalHistoryValid(
    FrameRenderer& renderer) {
    renderer.m_impl->taa.historyValid = true;
}

glm::vec2 FrameRendererTestAccess::nextTemporalJitter(
    FrameRenderer& renderer,
    int width,
    int height,
    float scale) {
    return renderer.m_impl->taa.jitter.next(width, height, scale);
}

} // namespace Rigel::Render
