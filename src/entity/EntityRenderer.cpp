#include "Rigel/Entity/EntityRenderer.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Entity/Aabb.h"
#include "Rigel/Voxel/World.h"
#include "Rigel/Asset/AssetManager.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace Rigel::Entity {

namespace {

bool isOpaqueBlock(const Voxel::World& world, int wx, int wy, int wz) {
    Voxel::BlockState state = world.getBlock(wx, wy, wz);
    if (state.isAir()) {
        return false;
    }
    const Voxel::BlockType& type = world.blockRegistry().getType(state.id);
    return type.isOpaque;
}

float computeEntityAo(const Voxel::World& world, const Aabb& bounds) {
    glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
    glm::ivec3 base = glm::ivec3(glm::floor(center));
    int solid = 0;
    int samples = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ++samples;
                if (isOpaqueBlock(world, base.x + dx, base.y + dy, base.z + dz)) {
                    ++solid;
                }
            }
        }
    }
    float occlusion = samples > 0 ? static_cast<float>(solid) / static_cast<float>(samples) : 0.0f;
    float ao = 1.0f - occlusion * 0.5f;
    return std::clamp(ao, 0.3f, 1.0f);
}

} // namespace

void EntityRenderer::initialize(Asset::AssetManager& assets) {
    m_assets = &assets;
    if (assets.exists("shaders/entity")) {
        m_shader = assets.get<Asset::ShaderAsset>("shaders/entity");
    } else {
        spdlog::warn("EntityRenderer: entity shader not found in manifest");
    }
    if (assets.exists("shaders/entity_shadow_depth")) {
        m_shadowShader = assets.get<Asset::ShaderAsset>("shaders/entity_shadow_depth");
    } else {
        spdlog::warn("EntityRenderer: entity shadow shader not found in manifest");
    }
}

void EntityRenderer::render(Voxel::World& world, const EntityRenderContext& ctx) {
    if (!m_assets || !m_shader) {
        return;
    }

    std::array<glm::vec4, 6> planes = extractPlanes(ctx.viewProjection);

    world.entities().forEach([&](Entity& entity) {
        const Aabb& bounds = entity.worldBounds();
        bool visible = isVisible(bounds, planes);

        if (!entity.ensureModelInstance(*m_assets, m_shader)) {
            return;
        }

        glm::mat4 modelMatrix = glm::translate(glm::mat4(1.0f), entity.position());
        EntityRenderContext localCtx = ctx;
        localCtx.ambientOcclusion = computeEntityAo(world, bounds);
        entity.render(localCtx, modelMatrix, visible);
    });
}

void EntityRenderer::renderShadowCasters(Voxel::World& world,
                                         const EntityRenderContext& ctx,
                                         const Voxel::ShadowCascadeContext& shadowCtx) {
    if (!m_assets || !m_shader || !m_shadowShader) {
        return;
    }

    std::array<glm::vec4, 6> planes = extractPlanes(shadowCtx.lightViewProjection);

    world.entities().forEach([&](Entity& entity) {
        const Aabb& bounds = entity.worldBounds();
        bool visible = isVisible(bounds, planes);

        if (!entity.ensureModelInstance(*m_assets, m_shader)) {
            return;
        }

        auto* instance = entity.modelInstance();
        if (!instance) {
            return;
        }

        glm::mat4 modelMatrix = glm::translate(glm::mat4(1.0f), entity.position());
        instance->renderShadow(ctx, entity, modelMatrix, shadowCtx.lightViewProjection,
                               m_shadowShader, visible);
    });
}

bool EntityRenderer::isVisible(const Aabb& bounds, const glm::mat4& viewProjection) {
    return isVisible(bounds, extractPlanes(viewProjection));
}

std::array<glm::vec4, 6> EntityRenderer::extractPlanes(const glm::mat4& viewProjection) {
    glm::vec4 row0 = glm::row(viewProjection, 0);
    glm::vec4 row1 = glm::row(viewProjection, 1);
    glm::vec4 row2 = glm::row(viewProjection, 2);
    glm::vec4 row3 = glm::row(viewProjection, 3);

    std::array<glm::vec4, 6> planes = {
        row3 + row0,
        row3 - row0,
        row3 + row1,
        row3 - row1,
        row3 + row2,
        row3 - row2
    };

    for (glm::vec4& plane : planes) {
        glm::vec3 normal(plane);
        float length = glm::length(normal);
        if (length > 0.0f) {
            plane /= length;
        }
    }

    return planes;
}

bool EntityRenderer::isVisible(const Aabb& bounds, const std::array<glm::vec4, 6>& planes) {
    for (const glm::vec4& plane : planes) {
        glm::vec3 normal(plane);
        glm::vec3 positive = bounds.min;
        if (normal.x >= 0.0f) {
            positive.x = bounds.max.x;
        }
        if (normal.y >= 0.0f) {
            positive.y = bounds.max.y;
        }
        if (normal.z >= 0.0f) {
            positive.z = bounds.max.z;
        }
        float distance = glm::dot(normal, positive) + plane.w;
        if (distance < 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace Rigel::Entity
