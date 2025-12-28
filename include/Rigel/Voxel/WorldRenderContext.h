#pragma once

#include "WorldMeshStore.h"
#include "TextureAtlas.h"

#include <Rigel/Asset/Handle.h>
#include <Rigel/Asset/Types.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Rigel::Voxel {

struct WorldRenderConfig {
    float renderDistance = 256.0f;
    glm::vec3 sunDirection = glm::vec3(0.5f, 1.0f, 0.3f);
    float transparentAlpha = 0.5f;
};

struct WorldRenderContext {
    const WorldMeshStore* meshes = nullptr;
    const TextureAtlas* atlas = nullptr;
    Asset::Handle<Asset::ShaderAsset> shader;
    WorldRenderConfig config;
    glm::mat4 viewProjection{1.0f};
    glm::vec3 cameraPos{0.0f};
    glm::mat4 worldTransform{1.0f};
};

} // namespace Rigel::Voxel
