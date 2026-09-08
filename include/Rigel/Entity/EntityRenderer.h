#pragma once

#include "EntityRenderContext.h"
#include "EntityId.h"
#include "EntityModel.h"
#include "EntityModelInstance.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Voxel/WorldRenderContext.h>

#include <glm/mat4x4.hpp>

#include <array>
#include <memory>
#include <unordered_map>

namespace Rigel::Asset {
class AssetManager;
struct ShaderAsset;
}

namespace Rigel::Voxel {
class World;
}

namespace Rigel::Entity {

struct Aabb;
class Entity;
namespace detail {
struct EntityRendererTestAccess;
}

class EntityRenderer {
public:
    EntityRenderer() = default;
    ~EntityRenderer();

    void initialize(Asset::AssetManager& assets);
    void render(Voxel::World& world, const EntityRenderContext& ctx);
    void renderShadowCasters(Voxel::World& world,
                             const EntityRenderContext& ctx,
                             const Voxel::ShadowCascadeContext& shadowCtx);
    void clear();
    void release();

    static bool isVisible(const Aabb& bounds, const glm::mat4& viewProjection);

private:
    struct RenderEntry {
        Asset::Handle<EntityModelAsset> model;
        std::unique_ptr<EntityModelInstance> instance;
    };

    EntityModelInstance* instanceFor(Entity& entity);
    void prune(const Voxel::World& world);
    static std::array<glm::vec4, 6> extractPlanes(const glm::mat4& viewProjection);
    static bool isVisible(const Aabb& bounds, const std::array<glm::vec4, 6>& planes);

    Asset::AssetManager* m_assets = nullptr;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    Asset::Handle<Asset::ShaderAsset> m_shadowShader;
    std::unordered_map<EntityId, RenderEntry, EntityIdHash> m_instances;

    friend struct detail::EntityRendererTestAccess;
};

} // namespace Rigel::Entity
