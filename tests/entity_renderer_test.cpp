#include "TestFramework.h"

#include "Rigel/Entity/EntityRenderer.h"
#include "Rigel/Entity/Aabb.h"

#include <glm/mat4x4.hpp>

using namespace Rigel::Entity;

TEST_CASE(EntityRenderer_CullsOutsideFrustum) {
    Aabb inside;
    inside.min = glm::vec3(-0.5f);
    inside.max = glm::vec3(0.5f);

    Aabb outside;
    outside.min = glm::vec3(2.0f, 2.0f, 2.0f);
    outside.max = glm::vec3(3.0f, 3.0f, 3.0f);

    glm::mat4 viewProjection(1.0f);

    CHECK(EntityRenderer::isVisible(inside, viewProjection));
    CHECK(!EntityRenderer::isVisible(outside, viewProjection));
}
