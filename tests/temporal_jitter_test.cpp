#include "TestFramework.h"

#include "Rigel/Render/TemporalJitter.h"

using Rigel::Render::TemporalJitterSequence;

TEST_CASE(TemporalJitter_FollowsHaltonSequence) {
    TemporalJitterSequence jitter;

    glm::vec2 first = jitter.next(800, 600, 1.0f);
    CHECK_NEAR(first.x, 0.0f, 0.0000001f);
    CHECK_NEAR(first.y, -1.0f / 1800.0f, 0.0000001f);

    glm::vec2 second = jitter.next(800, 600, 1.0f);
    CHECK_NEAR(second.x, -1.0f / 1600.0f, 0.0000001f);
    CHECK_NEAR(second.y, 1.0f / 1800.0f, 0.0000001f);
}

TEST_CASE(TemporalJitter_IgnoresInvalidViewport) {
    TemporalJitterSequence jitter;

    glm::vec2 invalid = jitter.next(0, 600, 1.0f);
    CHECK_NEAR(invalid.x, 0.0f, 0.0000001f);
    CHECK_NEAR(invalid.y, 0.0f, 0.0000001f);

    glm::vec2 first = jitter.next(800, 600, 1.0f);
    CHECK_NEAR(first.x, 0.0f, 0.0000001f);
    CHECK_NEAR(first.y, -1.0f / 1800.0f, 0.0000001f);
}
