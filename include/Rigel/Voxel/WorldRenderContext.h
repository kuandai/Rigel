#pragma once

#include "RenderConfig.h"
#include "TextureAtlas.h"
#include "WorldMeshStore.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Asset/Types.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Rigel::Voxel {

struct WorldRenderContext {
    const WorldMeshStore* meshes = nullptr;
    const TextureAtlas* atlas = nullptr;
    Asset::Handle<Asset::ShaderAsset> shader;
    Asset::Handle<Asset::ShaderAsset> shadowDepthShader;
    Asset::Handle<Asset::ShaderAsset> shadowTransmitShader;
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
