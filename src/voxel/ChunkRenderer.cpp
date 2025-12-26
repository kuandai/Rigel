#include "Rigel/Voxel/ChunkRenderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <vector>

namespace Rigel::Voxel {

ChunkRenderer::ChunkRenderer()
    : m_config{}
{
    m_config.sunDirection = glm::normalize(m_config.sunDirection);
}

ChunkRenderer::ChunkRenderer(const Config& config)
    : m_config(config)
{
    m_config.sunDirection = glm::normalize(m_config.sunDirection);
}

void ChunkRenderer::setShader(Asset::Handle<Asset::ShaderAsset> shader) {
    m_shader = std::move(shader);
    if (m_shader) {
        cacheUniformLocations();
    }
}

void ChunkRenderer::setTextureAtlas(TextureAtlas* atlas) {
    m_atlas = atlas;
}

void ChunkRenderer::setChunkMesh(ChunkCoord coord, ChunkMesh mesh) {
    // Upload to GPU if not already
    if (!mesh.isEmpty() && !mesh.isUploaded()) {
        mesh.uploadToGPU();
    }
    mesh.clearCPUData();
    m_meshes[coord] = std::move(mesh);
}

void ChunkRenderer::removeChunkMesh(ChunkCoord coord) {
    m_meshes.erase(coord);
}

bool ChunkRenderer::hasChunkMesh(ChunkCoord coord) const {
    return m_meshes.find(coord) != m_meshes.end();
}

void ChunkRenderer::clear() {
    m_meshes.clear();
}

void ChunkRenderer::render(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    if (!m_shader || m_meshes.empty()) {
        return;
    }

    // Bind shader
    m_shader->bind();

    // Set view-projection matrix
    if (m_locViewProjection >= 0) {
        glUniformMatrix4fv(m_locViewProjection, 1, GL_FALSE, glm::value_ptr(viewProjection));
    }

    // Set sun direction
    if (m_locSunDirection >= 0) {
        glUniform3fv(m_locSunDirection, 1, glm::value_ptr(m_config.sunDirection));
    }

    // Bind texture atlas
    if (m_atlas && m_locTextureAtlas >= 0) {
        m_atlas->bind(0);
        glUniform1i(m_locTextureAtlas, 0);
    }

    // Render each pass
    renderPass(RenderLayer::Opaque, viewProjection, cameraPos);
    renderPass(RenderLayer::Cutout, viewProjection, cameraPos);
    renderPass(RenderLayer::Transparent, viewProjection, cameraPos);
    renderPass(RenderLayer::Emissive, viewProjection, cameraPos);

    // Cleanup - reset GL state to defaults
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

void ChunkRenderer::setSunDirection(const glm::vec3& dir) {
    m_config.sunDirection = glm::normalize(dir);
}

void ChunkRenderer::cacheUniformLocations() {
    if (!m_shader) return;

    m_locViewProjection = m_shader->uniform("u_viewProjection");
    m_locChunkOffset = m_shader->uniform("u_chunkOffset");
    m_locTextureAtlas = m_shader->uniform("u_textureAtlas");
    m_locSunDirection = m_shader->uniform("u_sunDirection");
    m_locAlphaMultiplier = m_shader->uniform("u_alphaMultiplier");
    m_locAlphaCutoff = m_shader->uniform("u_alphaCutoff");
}

void ChunkRenderer::renderPass(RenderLayer layer, const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    setupLayerState(layer);

    float alphaMultiplier = 1.0f;
    float alphaCutoff = 0.0f;
    if (layer == RenderLayer::Cutout) {
        alphaCutoff = 0.5f;
    } else if (layer == RenderLayer::Transparent) {
        alphaMultiplier = m_config.transparentAlpha;
    }

    if (m_locAlphaMultiplier >= 0) {
        glUniform1f(m_locAlphaMultiplier, alphaMultiplier);
    }
    if (m_locAlphaCutoff >= 0) {
        glUniform1f(m_locAlphaCutoff, alphaCutoff);
    }

    auto drawChunk = [&](ChunkCoord coord, ChunkMesh& mesh) {
        glm::vec3 chunkOffset = coord.toWorldMin();
        if (m_locChunkOffset >= 0) {
            glUniform3fv(m_locChunkOffset, 1, glm::value_ptr(chunkOffset));
        }
        mesh.drawLayer(layer);
    };

    if (layer == RenderLayer::Transparent) {
        struct TransparentEntry {
            ChunkCoord coord;
            ChunkMesh* mesh;
            float distance;
        };
        std::vector<TransparentEntry> entries;
        entries.reserve(m_meshes.size());

        for (auto& [coord, mesh] : m_meshes) {
            if (mesh.isEmpty() || !mesh.isUploaded()) {
                continue;
            }

            const auto& range = mesh.layers[static_cast<size_t>(layer)];
            if (range.isEmpty()) {
                continue;
            }

            glm::vec3 chunkCenter = coord.toWorldCenter();
            float distance = glm::length(chunkCenter - cameraPos);
            if (distance > m_config.renderDistance) {
                continue;
            }

            entries.push_back({coord, &mesh, distance});
        }

        std::sort(entries.begin(), entries.end(),
                  [](const TransparentEntry& a, const TransparentEntry& b) {
                      return a.distance > b.distance;
                  });

        for (const auto& entry : entries) {
            drawChunk(entry.coord, *entry.mesh);
        }
        return;
    }

    // Opaque/cutout/emissive order doesn't need sorting.
    for (auto& [coord, mesh] : m_meshes) {
        if (mesh.isEmpty() || !mesh.isUploaded()) {
            continue;
        }

        const auto& range = mesh.layers[static_cast<size_t>(layer)];
        if (range.isEmpty()) {
            continue;
        }

        glm::vec3 chunkCenter = coord.toWorldCenter();
        float distance = glm::length(chunkCenter - cameraPos);
        if (distance > m_config.renderDistance) {
            continue;
        }

        drawChunk(coord, mesh);
    }
}

void ChunkRenderer::setupLayerState(RenderLayer layer) {
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
            // Alpha testing is done in shader
            break;

        case RenderLayer::Transparent:
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);  // Don't write depth
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;

        case RenderLayer::Emissive:
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);  // Additive
            break;
    }
}

} // namespace Rigel::Voxel
