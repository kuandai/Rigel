#include "Rigel/Render/TemporalJitter.h"

namespace Rigel::Render {
namespace {

float halton(uint32_t index, uint32_t base) {
    float factor = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        factor /= static_cast<float>(base);
        result += factor * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

} // namespace

glm::vec2 TemporalJitterSequence::next(int width, int height, float scale) {
    if (width <= 0 || height <= 0) {
        return glm::vec2(0.0f);
    }

    ++m_frameIndex;
    float x = halton(static_cast<uint32_t>(m_frameIndex), 2) - 0.5f;
    float y = halton(static_cast<uint32_t>(m_frameIndex), 3) - 0.5f;
    return glm::vec2(
        x * scale * 2.0f / static_cast<float>(width),
        y * scale * 2.0f / static_cast<float>(height));
}

} // namespace Rigel::Render
