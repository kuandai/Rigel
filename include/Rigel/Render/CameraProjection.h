#pragma once

#include <glm/mat4x4.hpp>

namespace Rigel::Render {

glm::mat4 makeCameraProjection(float verticalFovDegrees,
                               float aspect,
                               float nearPlane,
                               float farPlane);

} // namespace Rigel::Render
