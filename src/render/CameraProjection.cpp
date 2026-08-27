#include "Rigel/Render/CameraProjection.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Rigel::Render {

glm::mat4 makeCameraProjection(float verticalFovDegrees,
                               float aspect,
                               float nearPlane,
                               float farPlane) {
    return glm::perspective(
        glm::radians(verticalFovDegrees), aspect, nearPlane, farPlane);
}

} // namespace Rigel::Render
