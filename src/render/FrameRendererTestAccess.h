#pragma once

#include <cstdint>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace Rigel::Render {

class FrameRenderer;

class FrameRendererTestAccess {
public:
    static double verticalFovDegrees(const FrameRenderer& renderer);
    static std::pair<glm::mat4, glm::mat4> cameraProjections(
        const FrameRenderer& renderer,
        float mainAspect,
        float nearPlane,
        float farPlane);
    static bool temporalHistoryValid(const FrameRenderer& renderer);
    static uint32_t temporalHistoryColorTexture(
        const FrameRenderer& renderer);
    static void markTemporalHistoryValid(FrameRenderer& renderer);
    static glm::vec2 nextTemporalJitter(
        FrameRenderer& renderer,
        int width,
        int height,
        float scale);
};

} // namespace Rigel::Render
