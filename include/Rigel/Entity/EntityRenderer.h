#pragma once

#include "EntityRenderContext.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Voxel/WorldRenderContext.h>

#include <glm/mat4x4.hpp>

#include <array>

namespace Rigel::Asset {
class AssetManager;
struct ShaderAsset;
}

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Entity {

struct Aabb;

class EntityRenderer {
public:
    void initialize(Asset::AssetManager& assets);
    void render(Voxel::World& world, const EntityRenderContext& ctx);
    void renderShadowCasters(Voxel::World& world,
                             const EntityRenderContext& ctx,
                             const Voxel::ShadowCascadeContext& shadowCtx);

    static bool isVisible(const Aabb& bounds, const glm::mat4& viewProjection);

private:
    static std::array<glm::vec4, 6> extractPlanes(const glm::mat4& viewProjection);
    static bool isVisible(const Aabb& bounds, const std::array<glm::vec4, 6>& planes);

    Asset::AssetManager* m_assets = nullptr;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    Asset::Handle<Asset::ShaderAsset> m_shadowShader;
};

} // namespace Rigel::Entity
