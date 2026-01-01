#include "Rigel/Voxel/ChunkRenderer.h"

#include "Rigel/Voxel/VoxelVertex.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Rigel::Voxel {

namespace {
glm::vec3 normalizeOrDefault(const glm::vec3& value) {
    float lengthSq = glm::dot(value, value);
    if (lengthSq <= 0.000001f) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return glm::normalize(value);
}

glm::vec3 pickUpVector(const glm::vec3& direction) {
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f) {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    return up;
}

std::array<glm::vec3, 8> getFrustumCornersWorld(const glm::mat4& invViewProj) {
    std::array<glm::vec3, 8> corners{};
    int index = 0;
    for (int z = 0; z < 2; ++z) {
        float ndcZ = z == 0 ? -1.0f : 1.0f;
        for (int y = 0; y < 2; ++y) {
            float ndcY = y == 0 ? -1.0f : 1.0f;
            for (int x = 0; x < 2; ++x) {
                float ndcX = x == 0 ? -1.0f : 1.0f;
                glm::vec4 corner = invViewProj * glm::vec4(ndcX, ndcY, ndcZ, 1.0f);
                corner /= corner.w;
                corners[index++] = glm::vec3(corner);
            }
        }
    }
    return corners;
}


} // namespace

void ChunkRenderer::GpuMesh::release() {
    if (vao != 0) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    if (vbo != 0) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (ebo != 0) {
        glDeleteBuffers(1, &ebo);
        ebo = 0;
    }
    indexCount = 0;
}

ChunkRenderer::GpuMesh::~GpuMesh() {
    release();
}

ChunkRenderer::GpuMesh::GpuMesh(GpuMesh&& other) noexcept
    : vao(other.vao)
    , vbo(other.vbo)
    , ebo(other.ebo)
    , indexCount(other.indexCount)
    , layers(other.layers)
{
    other.vao = 0;
    other.vbo = 0;
    other.ebo = 0;
    other.indexCount = 0;
}

ChunkRenderer::GpuMesh& ChunkRenderer::GpuMesh::operator=(GpuMesh&& other) noexcept {
    if (this != &other) {
        release();
        vao = other.vao;
        vbo = other.vbo;
        ebo = other.ebo;
        indexCount = other.indexCount;
        layers = other.layers;
        other.vao = 0;
        other.vbo = 0;
        other.ebo = 0;
        other.indexCount = 0;
    }
    return *this;
}

void ChunkRenderer::clearCache() {
    m_meshes.clear();
    m_storeVersions.clear();
}

void ChunkRenderer::releaseResources() {
    clearCache();
    releaseShadowResources();
}

void ChunkRenderer::uploadMesh(GpuMesh& gpu, const ChunkMesh& mesh) const {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        gpu.release();
        gpu.layers = {};
        return;
    }

    if (gpu.vao == 0) {
        glGenVertexArrays(1, &gpu.vao);
        glGenBuffers(1, &gpu.vbo);
        glGenBuffers(1, &gpu.ebo);
    }

    glBindVertexArray(gpu.vao);

    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(VoxelVertex)),
        mesh.vertices.data(),
        GL_STATIC_DRAW
    );

    VoxelVertex::setupAttributes();

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)),
        mesh.indices.data(),
        GL_STATIC_DRAW
    );

    glBindVertexArray(0);

    gpu.indexCount = mesh.indices.size();
    gpu.layers = mesh.layers;
}

void ChunkRenderer::pruneCache(const WorldMeshStore& store) {
    uint32_t storeId = store.storeId();
    for (auto it = m_meshes.begin(); it != m_meshes.end(); ) {
        if (it->first.storeId != storeId) {
            ++it;
            continue;
        }
        if (!store.contains(it->second.coord)) {
            it = m_meshes.erase(it);
            continue;
        }
        ++it;
    }
}

void ChunkRenderer::render(const WorldRenderContext& ctx) {
    m_shadowsActive = false;
    if (!ctx.meshes || !ctx.shader) {
        return;
    }

    if (m_shader != ctx.shader) {
        m_shader = ctx.shader;
        cacheUniformLocations();
    }
    if (m_shadowDepthShader != ctx.shadowDepthShader ||
        m_shadowTransmitShader != ctx.shadowTransmitShader) {
        m_shadowDepthShader = ctx.shadowDepthShader;
        m_shadowTransmitShader = ctx.shadowTransmitShader;
        cacheShadowUniforms();
    }
    m_atlas = ctx.atlas;

    uint32_t storeId = ctx.meshes->storeId();
    uint64_t version = ctx.meshes->version();
    auto versionIt = m_storeVersions.find(storeId);
    if (versionIt == m_storeVersions.end() || versionIt->second != version) {
        pruneCache(*ctx.meshes);
        m_storeVersions[storeId] = version;
    }

    float renderDistance = std::max(0.0f, ctx.config.renderDistance);
    float renderDistanceSq = renderDistance * renderDistance;

    glm::vec3 viewDir(-ctx.view[0][2], -ctx.view[1][2], -ctx.view[2][2]);
    viewDir = normalizeOrDefault(viewDir);

    std::vector<RenderEntry> entries;
    ctx.meshes->forEach([&](const WorldMeshEntry& entry) {
        if (entry.mesh.isEmpty()) {
            return;
        }

        auto meshIt = m_meshes.find(entry.id);
        if (meshIt == m_meshes.end()) {
            GpuMeshEntry gpuEntry;
            gpuEntry.coord = entry.coord;
            gpuEntry.revision = entry.revision;
            uploadMesh(gpuEntry.mesh, entry.mesh);
            meshIt = m_meshes.emplace(entry.id, std::move(gpuEntry)).first;
        } else if (meshIt->second.revision.value != entry.revision.value) {
            meshIt->second.coord = entry.coord;
            meshIt->second.revision = entry.revision;
            uploadMesh(meshIt->second.mesh, entry.mesh);
        }

        if (!meshIt->second.mesh.isValid()) {
            return;
        }

        glm::vec3 center = entry.coord.toWorldCenter();
        glm::vec3 worldCenter = glm::vec3(ctx.worldTransform * glm::vec4(center, 1.0f));
        glm::vec3 delta = worldCenter - ctx.cameraPos;
        float distanceSq = glm::dot(delta, delta);
        if (distanceSq > renderDistanceSq) {
            return;
        }

        float viewDepth = glm::dot(delta, viewDir);
        entries.push_back(RenderEntry{entry.coord, entry.id, distanceSq, viewDepth});
    });

    if (entries.empty()) {
        glUseProgram(0);
        return;
    }

    bool shadowsActive = renderShadows(ctx, entries);
    m_shadowsActive = shadowsActive;

    glm::mat4 viewProjection = ctx.viewProjection * ctx.worldTransform;
    glm::vec3 sunDirection = normalizeOrDefault(ctx.config.sunDirection);

    m_shader->bind();
    if (m_locViewProjection >= 0) {
        glUniformMatrix4fv(m_locViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    }
    if (m_locView >= 0) {
        glUniformMatrix4fv(m_locView, 1, GL_FALSE, glm::value_ptr(ctx.view));
    }
    if (m_locSunDirection >= 0) {
        glUniform3fv(m_locSunDirection, 1, glm::value_ptr(sunDirection));
    }
    GLint locCameraPos = m_shader->uniform("u_cameraPos");
    if (locCameraPos >= 0) {
        glUniform3fv(locCameraPos, 1, glm::value_ptr(ctx.cameraPos));
    }
    if (m_atlas && m_locTextureAtlas >= 0) {
        m_atlas->bind(0);
        glUniform1i(m_locTextureAtlas, 0);
    }

    if (m_locShadowEnabled >= 0) {
        glUniform1i(m_locShadowEnabled, shadowsActive ? 1 : 0);
    }
    if (shadowsActive) {
        constexpr int kShadowMapUnit = 1;
        constexpr int kShadowTransmitUnit = 2;

        if (m_locShadowMap >= 0) {
            glActiveTexture(GL_TEXTURE0 + kShadowMapUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowState.depthArray);
            glUniform1i(m_locShadowMap, kShadowMapUnit);
        }
        if (m_locShadowTransmittanceMap >= 0) {
            glActiveTexture(GL_TEXTURE0 + kShadowTransmitUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowState.transmitArray);
            glUniform1i(m_locShadowTransmittanceMap, kShadowTransmitUnit);
        }
        if (m_locShadowMatrices >= 0) {
            glUniformMatrix4fv(m_locShadowMatrices,
                               m_shadowState.cascades,
                               GL_FALSE,
                               glm::value_ptr(m_shadowState.matrices[0]));
        }
        if (m_locShadowSplits >= 0) {
            glUniform1fv(m_locShadowSplits,
                         m_shadowState.cascades,
                         m_shadowState.splits.data());
        }
        if (m_locShadowCascadeCount >= 0) {
            glUniform1i(m_locShadowCascadeCount, m_shadowState.cascades);
        }
        if (m_locShadowBias >= 0) {
            glUniform1f(m_locShadowBias, ctx.config.shadow.bias);
        }
        if (m_locShadowNormalBias >= 0) {
            glUniform1f(m_locShadowNormalBias, ctx.config.shadow.normalBias);
        }
        if (m_locShadowPcfRadius >= 0) {
            glUniform1i(m_locShadowPcfRadius, ctx.config.shadow.pcfRadius);
        }
        if (m_locShadowPcfNear >= 0) {
            glUniform1f(m_locShadowPcfNear,
                        static_cast<float>(ctx.config.shadow.pcfRadiusNear));
        }
        if (m_locShadowPcfFar >= 0) {
            glUniform1f(m_locShadowPcfFar,
                        static_cast<float>(ctx.config.shadow.pcfRadiusFar));
        }
        if (m_locShadowStrength >= 0) {
            glUniform1f(m_locShadowStrength, ctx.config.shadow.strength);
        }
        if (m_locShadowNear >= 0) {
            glUniform1f(m_locShadowNear, ctx.nearPlane);
        }
        if (m_locShadowFadeStart >= 0) {
            float fadeStart = ctx.config.shadow.maxDistance > 0.0f
                ? std::min(ctx.config.shadow.maxDistance, ctx.farPlane)
                : ctx.farPlane;
            glUniform1f(m_locShadowFadeStart, fadeStart);
        }
        if (m_locShadowFadePower >= 0) {
            glUniform1f(m_locShadowFadePower, ctx.config.shadow.fadePower);
        }
    } else if (m_locShadowCascadeCount >= 0) {
        glUniform1i(m_locShadowCascadeCount, 0);
    }

    renderPass(RenderLayer::Opaque, entries, ctx);
    renderPass(RenderLayer::Cutout, entries, ctx);
    renderPass(RenderLayer::Transparent, entries, ctx);
    renderPass(RenderLayer::Emissive, entries, ctx);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

ChunkRenderer::ShadowRenderState ChunkRenderer::shadowRenderState() const {
    ShadowRenderState state{};
    state.active = m_shadowsActive;
    state.depthArray = m_shadowState.depthArray;
    state.transmitArray = m_shadowState.transmitArray;
    state.cascades = m_shadowState.cascades;
    state.mapSize = m_shadowState.mapSize;
    state.matrices = m_shadowState.matrices;
    state.splits = m_shadowState.splits;
    return state;
}

void ChunkRenderer::cacheUniformLocations() {
    if (!m_shader) {
        return;
    }

    m_locViewProjection = m_shader->uniform("u_viewProjection");
    m_locView = m_shader->uniform("u_view");
    m_locChunkOffset = m_shader->uniform("u_chunkOffset");
    m_locTextureAtlas = m_shader->uniform("u_textureAtlas");
    m_locSunDirection = m_shader->uniform("u_sunDirection");
    m_locAlphaMultiplier = m_shader->uniform("u_alphaMultiplier");
    m_locAlphaCutoff = m_shader->uniform("u_alphaCutoff");
    m_locRenderLayer = m_shader->uniform("u_renderLayer");
    m_locShadowEnabled = m_shader->uniform("u_shadowEnabled");
    m_locShadowMap = m_shader->uniform("u_shadowMap");
    m_locShadowTransmittanceMap = m_shader->uniform("u_shadowTransmittanceMap");
    m_locShadowMatrices = m_shader->uniform("u_shadowMatrices");
    m_locShadowSplits = m_shader->uniform("u_shadowSplits");
    m_locShadowCascadeCount = m_shader->uniform("u_shadowCascadeCount");
    m_locShadowBias = m_shader->uniform("u_shadowBias");
    m_locShadowNormalBias = m_shader->uniform("u_shadowNormalBias");
    m_locShadowPcfRadius = m_shader->uniform("u_shadowPcfRadius");
    m_locShadowStrength = m_shader->uniform("u_shadowStrength");
    m_locShadowNear = m_shader->uniform("u_shadowNear");
    m_locShadowFadeStart = m_shader->uniform("u_shadowFadeStart");
    m_locShadowPcfNear = m_shader->uniform("u_shadowPcfNear");
    m_locShadowPcfFar = m_shader->uniform("u_shadowPcfFar");
    m_locShadowFadePower = m_shader->uniform("u_shadowFadePower");
}

void ChunkRenderer::cacheShadowUniforms() {
    m_shadowDepthUniforms = {};
    if (m_shadowDepthShader) {
        m_shadowDepthUniforms.lightViewProjection =
            m_shadowDepthShader->uniform("u_lightViewProjection");
        m_shadowDepthUniforms.chunkOffset =
            m_shadowDepthShader->uniform("u_chunkOffset");
        m_shadowDepthUniforms.textureAtlas =
            m_shadowDepthShader->uniform("u_textureAtlas");
        m_shadowDepthUniforms.alphaCutoff =
            m_shadowDepthShader->uniform("u_alphaCutoff");
    }

    m_shadowTransmitUniforms = {};
    if (m_shadowTransmitShader) {
        m_shadowTransmitUniforms.lightViewProjection =
            m_shadowTransmitShader->uniform("u_lightViewProjection");
        m_shadowTransmitUniforms.chunkOffset =
            m_shadowTransmitShader->uniform("u_chunkOffset");
        m_shadowTransmitUniforms.tintAtlas =
            m_shadowTransmitShader->uniform("u_shadowTintAtlas");
        m_shadowTransmitUniforms.transparentScale =
            m_shadowTransmitShader->uniform("u_transparentScale");
    }
}

void ChunkRenderer::renderPass(RenderLayer layer,
                               const std::vector<RenderEntry>& entries,
                               const WorldRenderContext& ctx) {
    setupLayerState(layer);

    float alphaMultiplier = 1.0f;
    float alphaCutoff = 0.0f;
    if (layer == RenderLayer::Cutout) {
        alphaCutoff = 0.5f;
    } else if (layer == RenderLayer::Transparent) {
        alphaMultiplier = ctx.config.transparentAlpha;
    }

    if (m_locAlphaMultiplier >= 0) {
        glUniform1f(m_locAlphaMultiplier, alphaMultiplier);
    }
    if (m_locAlphaCutoff >= 0) {
        glUniform1f(m_locAlphaCutoff, alphaCutoff);
    }
    if (m_locRenderLayer >= 0) {
        glUniform1i(m_locRenderLayer, static_cast<int>(layer));
    }

    auto drawEntry = [&](const RenderEntry& entry) {
        auto meshIt = m_meshes.find(entry.meshId);
        if (meshIt == m_meshes.end()) {
            return;
        }
        const GpuMesh& mesh = meshIt->second.mesh;
        if (!mesh.isValid()) {
            return;
        }
        const auto& range = mesh.layers[static_cast<size_t>(layer)];
        if (range.isEmpty()) {
            return;
        }

        glm::vec3 chunkOffset = entry.coord.toWorldMin();
        if (m_locChunkOffset >= 0) {
            glUniform3fv(m_locChunkOffset, 1, glm::value_ptr(chunkOffset));
        }

        glBindVertexArray(mesh.vao);
        glDrawElements(
            GL_TRIANGLES,
            static_cast<GLsizei>(range.indexCount),
            GL_UNSIGNED_INT,
            reinterpret_cast<void*>(static_cast<uintptr_t>(range.indexStart * sizeof(uint32_t)))
        );
        glBindVertexArray(0);
    };

    if (layer == RenderLayer::Transparent) {
        std::vector<RenderEntry> sorted = entries;
        std::sort(sorted.begin(), sorted.end(),
                  [](const RenderEntry& a, const RenderEntry& b) {
                      return a.viewDepth > b.viewDepth;
                  });
        for (const auto& entry : sorted) {
            drawEntry(entry);
        }
        return;
    }

    for (const auto& entry : entries) {
        drawEntry(entry);
    }
}

void ChunkRenderer::setupLayerState(RenderLayer layer) const {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);

    switch (layer) {
        case RenderLayer::Opaque:
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            break;

        case RenderLayer::Cutout:
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            break;

        case RenderLayer::Transparent:
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;

        case RenderLayer::Emissive:
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            break;
    }
}

void ChunkRenderer::releaseShadowResources() {
    if (m_shadowState.fbo != 0) {
        glDeleteFramebuffers(1, &m_shadowState.fbo);
        m_shadowState.fbo = 0;
    }
    if (m_shadowState.depthArray != 0) {
        glDeleteTextures(1, &m_shadowState.depthArray);
        m_shadowState.depthArray = 0;
    }
    if (m_shadowState.transmitArray != 0) {
        glDeleteTextures(1, &m_shadowState.transmitArray);
        m_shadowState.transmitArray = 0;
    }
    m_shadowState.cascades = 0;
    m_shadowState.mapSize = 0;
    m_shadowState.matrices = {};
    m_shadowState.splits = {};
}

bool ChunkRenderer::ensureShadowResources(const ShadowConfig& config) {
    int cascades = std::clamp(config.cascades, 1, kMaxShadowCascades);
    int mapSize = std::max(1, config.mapSize);

    if (m_shadowState.depthArray != 0 &&
        m_shadowState.transmitArray != 0 &&
        m_shadowState.fbo != 0 &&
        m_shadowState.cascades == cascades &&
        m_shadowState.mapSize == mapSize) {
        return true;
    }

    releaseShadowResources();

    m_shadowState.cascades = cascades;
    m_shadowState.mapSize = mapSize;

    glGenTextures(1, &m_shadowState.depthArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowState.depthArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
                 mapSize, mapSize, cascades, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderDepth[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderDepth);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    glGenTextures(1, &m_shadowState.transmitArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowState.transmitArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                 mapSize, mapSize, cascades, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    glGenFramebuffers(1, &m_shadowState.fbo);
    return true;
}

bool ChunkRenderer::renderShadows(const WorldRenderContext& ctx,
                                  const std::vector<RenderEntry>& entries) {
    const ShadowConfig& shadow = ctx.config.shadow;
    if (!shadow.enabled || !m_shadowDepthShader || !m_atlas) {
        static int skipLogCounter = 0;
        if (skipLogCounter++ < 3) {
            spdlog::info(
                "Shadow pass skipped: enabled={}, depthShader={}, atlas={}",
                shadow.enabled ? 1 : 0,
                static_cast<bool>(m_shadowDepthShader),
                static_cast<bool>(m_atlas)
            );
        }
        return false;
    }
    if (!ensureShadowResources(shadow)) {
        return false;
    }

    static int drawLogCounter = 0;
    if (drawLogCounter++ < 3) {
        int opaqueDraws = countShadowDraws(entries, RenderLayer::Opaque);
        int cutoutDraws = countShadowDraws(entries, RenderLayer::Cutout);
        int transparentDraws = countShadowDraws(entries, RenderLayer::Transparent);
        spdlog::info(
            "Shadow draw counts: opaque={}, cutout={}, transparent={}, cascades={}, mapSize={}",
            opaqueDraws,
            cutoutDraws,
            transparentDraws,
            m_shadowState.cascades,
            m_shadowState.mapSize
        );
    }

    int cascades = m_shadowState.cascades;
    float nearPlane = std::max(ctx.nearPlane, 0.01f);
    float farPlane = std::max(ctx.farPlane, nearPlane + 0.1f);
    float shadowFar = std::max(farPlane, nearPlane + 0.1f);

    float shadowClipRange = shadowFar - nearPlane;
    float ratio = shadowFar / nearPlane;
    float splitLambda = std::clamp(shadow.splitLambda, 0.0f, 1.0f);
    for (int i = 0; i < cascades; ++i) {
        float p = static_cast<float>(i + 1) / static_cast<float>(cascades);
        float logSplit = nearPlane * std::pow(ratio, p);
        float uniSplit = nearPlane + shadowClipRange * p;
        float split = glm::mix(uniSplit, logSplit, splitLambda);
        m_shadowState.splits[i] = split;
    }

    glm::vec3 sunDir = normalizeOrDefault(ctx.config.sunDirection);
    glm::vec3 cameraPos = ctx.cameraPos;

    for (int i = 0; i < cascades; ++i) {
        float cascadeNear = (i == 0) ? nearPlane : m_shadowState.splits[i - 1];
        float cascadeFar = m_shadowState.splits[i];
        float radius = std::max(cascadeFar, cascadeNear);
        glm::vec3 centerWorld = cameraPos;

        float lightDistance = radius + 10.0f;
        glm::vec3 lightPos = centerWorld + sunDir * lightDistance;
        glm::mat4 lightView = glm::lookAt(lightPos, centerWorld, pickUpVector(sunDir));

        glm::vec3 centerLS = glm::vec3(lightView * glm::vec4(centerWorld, 1.0f));
        float extent = radius * 2.0f;
        if (extent > 0.0f) {
            float texelSize = extent / static_cast<float>(m_shadowState.mapSize);
            if (texelSize > 0.0f) {
                centerLS.x = std::floor(centerLS.x / texelSize) * texelSize;
                centerLS.y = std::floor(centerLS.y / texelSize) * texelSize;
            }
        }

        float minX = centerLS.x - radius;
        float maxX = centerLS.x + radius;
        float minY = centerLS.y - radius;
        float maxY = centerLS.y + radius;
        float minZ = centerLS.z - radius;
        float maxZ = centerLS.z + radius;

        float zPadding = 20.0f;
        float nearZ = -maxZ - zPadding;
        float farZ = -minZ + zPadding;
        glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, nearZ, farZ);
        m_shadowState.matrices[i] = lightProj * lightView;
    }

    bool hasTransparent = false;
    for (const auto& entry : entries) {
        auto meshIt = m_meshes.find(entry.meshId);
        if (meshIt == m_meshes.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second.mesh;
        if (mesh.layers[static_cast<size_t>(RenderLayer::Transparent)].isEmpty()) {
            continue;
        }
        hasTransparent = true;
        break;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    glViewport(0, 0, m_shadowState.mapSize, m_shadowState.mapSize);

    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowState.fbo);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    if (!hasTransparent || shadow.transparentScale <= 0.0f || !m_shadowTransmitShader) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        for (int cascade = 0; cascade < cascades; ++cascade) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      m_shadowState.transmitArray, 0, cascade);
            GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &drawBuffer);
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }

    for (int cascade = 0; cascade < cascades; ++cascade) {
        m_shadowDepthShader->bind();
        if (m_shadowDepthUniforms.textureAtlas >= 0) {
            m_atlas->bind(0);
            glUniform1i(m_shadowDepthUniforms.textureAtlas, 0);
        }

        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  m_shadowState.depthArray, 0, cascade);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glClear(GL_DEPTH_BUFFER_BIT);

        if (cascade == 0) {
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                spdlog::warn("Shadow depth FBO incomplete: status=0x{:X}", status);
            }
        }

        if (m_shadowDepthUniforms.lightViewProjection >= 0) {
            glUniformMatrix4fv(m_shadowDepthUniforms.lightViewProjection,
                               1,
                               GL_FALSE,
                               glm::value_ptr(m_shadowState.matrices[cascade]));
        }

        if (m_shadowDepthUniforms.alphaCutoff >= 0) {
            glUniform1f(m_shadowDepthUniforms.alphaCutoff, 0.0f);
        }
        renderShadowLayer(entries, RenderLayer::Opaque, ctx, m_shadowDepthUniforms);

        if (m_shadowDepthUniforms.alphaCutoff >= 0) {
            glUniform1f(m_shadowDepthUniforms.alphaCutoff, 0.5f);
        }
        renderShadowLayer(entries, RenderLayer::Cutout, ctx, m_shadowDepthUniforms);

        if (ctx.shadowCaster) {
            ShadowCascadeContext shadowCtx;
            shadowCtx.cascade = cascade;
            shadowCtx.lightViewProjection = m_shadowState.matrices[cascade];
            ctx.shadowCaster->renderShadowCascade(shadowCtx);

            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
        }
    }

    if (hasTransparent && shadow.transparentScale > 0.0f && m_shadowTransmitShader) {
        m_shadowTransmitShader->bind();
        if (m_shadowTransmitUniforms.tintAtlas >= 0) {
            m_atlas->bindTint(0);
            glUniform1i(m_shadowTransmitUniforms.tintAtlas, 0);
        }
        if (m_shadowTransmitUniforms.transparentScale >= 0) {
            glUniform1f(m_shadowTransmitUniforms.transparentScale, shadow.transparentScale);
        }

        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        glBlendEquation(GL_FUNC_ADD);

        for (int cascade = 0; cascade < cascades; ++cascade) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      m_shadowState.depthArray, 0, cascade);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      m_shadowState.transmitArray, 0, cascade);
            GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &drawBuffer);
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (cascade == 0) {
                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    spdlog::warn("Shadow transmit FBO incomplete: status=0x{:X}", status);
                }
            }

            if (m_shadowTransmitUniforms.lightViewProjection >= 0) {
                glUniformMatrix4fv(m_shadowTransmitUniforms.lightViewProjection,
                                   1,
                                   GL_FALSE,
                                   glm::value_ptr(m_shadowState.matrices[cascade]));
            }

            renderShadowLayer(entries, RenderLayer::Transparent, ctx, m_shadowTransmitUniforms);
        }

        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glUseProgram(0);
    return true;
}

void ChunkRenderer::renderShadowLayer(const std::vector<RenderEntry>& entries,
                                      RenderLayer layer,
                                      const WorldRenderContext& ctx,
                                      const ShadowUniforms& uniforms) const {
    (void)ctx;
    for (const auto& entry : entries) {
        auto meshIt = m_meshes.find(entry.meshId);
        if (meshIt == m_meshes.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second.mesh;
        if (!mesh.isValid()) {
            continue;
        }
        const auto& range = mesh.layers[static_cast<size_t>(layer)];
        if (range.isEmpty()) {
            continue;
        }

        glm::vec3 chunkOffset = entry.coord.toWorldMin();
        if (uniforms.chunkOffset >= 0) {
            glUniform3fv(uniforms.chunkOffset, 1, glm::value_ptr(chunkOffset));
        }

        glBindVertexArray(mesh.vao);
        glDrawElements(
            GL_TRIANGLES,
            static_cast<GLsizei>(range.indexCount),
            GL_UNSIGNED_INT,
            reinterpret_cast<void*>(static_cast<uintptr_t>(range.indexStart * sizeof(uint32_t)))
        );
        glBindVertexArray(0);
    }
}

int ChunkRenderer::countShadowDraws(const std::vector<RenderEntry>& entries,
                                    RenderLayer layer) const {
    int count = 0;
    for (const auto& entry : entries) {
        auto meshIt = m_meshes.find(entry.meshId);
        if (meshIt == m_meshes.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second.mesh;
        if (!mesh.isValid()) {
            continue;
        }
        const auto& range = mesh.layers[static_cast<size_t>(layer)];
        if (range.isEmpty()) {
            continue;
        }
        ++count;
    }
    return count;
}

} // namespace Rigel::Voxel
