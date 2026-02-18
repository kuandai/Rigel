#pragma once

#include "RenderConfig.h"
#include "VoxelLod/VoxelSvoLodManager.h"
#include "TextureAtlas.h"
#include "WorldMeshStore.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Asset/Types.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Rigel::Voxel {

struct ShadowCascadeContext {
    int cascade = 0;
    glm::mat4 lightViewProjection{1.0f};
};

class IShadowCaster {
public:
    virtual ~IShadowCaster() = default;
    virtual void renderShadowCascade(const ShadowCascadeContext& ctx) = 0;
};

struct WorldRenderContext {
    const WorldMeshStore* meshes = nullptr;
    const TextureAtlas* atlas = nullptr;
    Asset::Handle<Asset::ShaderAsset> shader;
    Asset::Handle<Asset::ShaderAsset> shadowDepthShader;
    Asset::Handle<Asset::ShaderAsset> shadowTransmitShader;
    VoxelSvoLodManager* voxelSvoLod = nullptr;
    IShadowCaster* shadowCaster = nullptr;
    WorldRenderConfig config;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 viewProjection{1.0f};
    glm::vec3 cameraPos{0.0f};
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    glm::mat4 worldTransform{1.0f};
};

} // namespace Rigel::Voxel
