#pragma once

#include <cstdint>

#include <glm/vec2.hpp>

namespace Rigel::Render {

class TemporalJitterSequence {
public:
    glm::vec2 next(int width, int height, float scale);
    void reset() { m_frameIndex = 0; }

private:
    uint64_t m_frameIndex = 0;
};

} // namespace Rigel::Render
