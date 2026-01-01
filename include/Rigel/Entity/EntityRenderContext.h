#pragma once

#include <Rigel/Voxel/RenderConfig.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <array>
#include <cstdint>
#include <GL/glew.h>

namespace Rigel::Entity {

struct EntityShadowContext {
    bool enabled = false;
    GLuint depthMap = 0;
    GLuint transmittanceMap = 0;
    int cascadeCount = 0;
    std::array<glm::mat4, Voxel::ShadowConfig::MaxCascades> matrices{};
    std::array<float, Voxel::ShadowConfig::MaxCascades> splits{};
    float bias = 0.0f;
    float normalBias = 0.0f;
    int pcfRadius = 0;
    float pcfNear = 0.0f;
    float pcfFar = 0.0f;
    float strength = 1.0f;
    float nearPlane = 0.1f;
    float fadeStart = 0.0f;
    float fadePower = 1.0f;
};

struct EntityRenderContext {
    glm::mat4 viewProjection{1.0f};
    glm::mat4 view{1.0f};
    glm::vec3 cameraPos{0.0f};
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};
    float ambientStrength = 0.3f;
    float deltaTime = 0.0f;
    uint64_t frameIndex = 0;
    float ambientOcclusion = 1.0f;
    EntityShadowContext shadow;
};

} // namespace Rigel::Entity
