#include "Rigel/Voxel/ChunkRenderer.h"

#include "Rigel/Voxel/VoxelVertex.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
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
    if (!ctx.meshes || !ctx.shader) {
        return;
    }

    if (m_shader != ctx.shader) {
        m_shader = ctx.shader;
        cacheUniformLocations();
    }
    m_atlas = ctx.atlas;

    uint32_t storeId = ctx.meshes->storeId();
    uint64_t version = ctx.meshes->version();
    auto versionIt = m_storeVersions.find(storeId);
    if (versionIt == m_storeVersions.end() || versionIt->second != version) {
        pruneCache(*ctx.meshes);
        m_storeVersions[storeId] = version;
    }

    glm::mat4 viewProjection = ctx.viewProjection * ctx.worldTransform;
    glm::vec3 sunDirection = normalizeOrDefault(ctx.config.sunDirection);

    m_shader->bind();
    if (m_locViewProjection >= 0) {
        glUniformMatrix4fv(m_locViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    }
    if (m_locSunDirection >= 0) {
        glUniform3fv(m_locSunDirection, 1, glm::value_ptr(sunDirection));
    }
    if (m_atlas && m_locTextureAtlas >= 0) {
        m_atlas->bind(0);
        glUniform1i(m_locTextureAtlas, 0);
    }

    float renderDistance = std::max(0.0f, ctx.config.renderDistance);
    float renderDistanceSq = renderDistance * renderDistance;

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

        entries.push_back(RenderEntry{entry.coord, entry.id, distanceSq});
    });

    if (entries.empty()) {
        glUseProgram(0);
        return;
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

void ChunkRenderer::cacheUniformLocations() {
    if (!m_shader) {
        return;
    }

    m_locViewProjection = m_shader->uniform("u_viewProjection");
    m_locChunkOffset = m_shader->uniform("u_chunkOffset");
    m_locTextureAtlas = m_shader->uniform("u_textureAtlas");
    m_locSunDirection = m_shader->uniform("u_sunDirection");
    m_locAlphaMultiplier = m_shader->uniform("u_alphaMultiplier");
    m_locAlphaCutoff = m_shader->uniform("u_alphaCutoff");
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
                      return a.distanceSq > b.distanceSq;
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

} // namespace Rigel::Voxel
