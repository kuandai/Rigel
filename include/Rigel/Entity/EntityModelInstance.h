#pragma once

#include "EntityModel.h"
#include "EntityRenderContext.h"

#include <Rigel/Asset/Handle.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <GL/glew.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Asset {
struct TextureAsset;
struct ShaderAsset;
}

namespace Rigel::Entity {

class Entity;

class IEntityModelInstance {
public:
    virtual ~IEntityModelInstance() = default;

    virtual void render(const EntityRenderContext& ctx,
                        Entity& entity,
                        const glm::mat4& modelMatrix,
                        bool shouldRender) = 0;
    virtual void renderShadow(const EntityRenderContext& ctx,
                              Entity& entity,
                              const glm::mat4& modelMatrix,
                              const glm::mat4& lightViewProjection,
                              const Asset::Handle<Asset::ShaderAsset>& shadowShader,
                              bool shouldRender) = 0;

    virtual void setTint(const glm::vec4& tint) = 0;
    virtual void addAnimation(std::string_view name) = 0;
    virtual void removeAnimation(std::string_view name) = 0;
    virtual void removeAnimation(const EntityAnimation* animation) = 0;

    virtual const EntityModelAsset* model() const = 0;
};

class EntityModelInstance : public IEntityModelInstance {
public:
    EntityModelInstance(std::shared_ptr<const EntityModelAsset> model,
                        Asset::Handle<Asset::ShaderAsset> shader,
                        std::unordered_map<std::string, Asset::Handle<Asset::TextureAsset>> textures);
    ~EntityModelInstance() override;

    void render(const EntityRenderContext& ctx,
                Entity& entity,
                const glm::mat4& modelMatrix,
                bool shouldRender) override;
    void renderShadow(const EntityRenderContext& ctx,
                      Entity& entity,
                      const glm::mat4& modelMatrix,
                      const glm::mat4& lightViewProjection,
                      const Asset::Handle<Asset::ShaderAsset>& shadowShader,
                      bool shouldRender) override;

    void setTint(const glm::vec4& tint) override { m_tint = tint; }
    void addAnimation(std::string_view name) override;
    void removeAnimation(std::string_view name) override;
    void removeAnimation(const EntityAnimation* animation) override;

    const EntityModelAsset* model() const override { return m_model.get(); }

private:
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::vec2 uv{0.0f};
    };

    void rebuildMesh(const EntityRenderContext& ctx);
    void updateAnimations(const EntityRenderContext& ctx);
    glm::vec3 sampleTrack(const EntityAnimationTrack& track,
                          const EntityAnimation& animation,
                          const glm::vec3& defaultValue) const;

    void ensureGpuResources();
    void releaseGpuResources();

    std::shared_ptr<const EntityModelAsset> m_model;
    const EntityAnimationSet* m_animationSet = nullptr;
    Asset::Handle<Asset::ShaderAsset> m_shader;
    std::unordered_map<std::string, Asset::Handle<Asset::TextureAsset>> m_textures;

    glm::vec4 m_tint{1.0f};
    std::vector<const EntityAnimation*> m_activeAnimations;
    float m_globalAnimTime = 0.0f;
    uint64_t m_lastAnimFrame = 0;

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    size_t m_vertexCount = 0;
    std::vector<Vertex> m_cpuVertices;
    bool m_meshDirty = true;
};

} // namespace Rigel::Entity
