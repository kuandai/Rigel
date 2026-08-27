#pragma once

#include <cstdint>

namespace Rigel::Render {

class FrameRenderer;

class FrameRendererTestAccess {
public:
    static double verticalFovDegrees(const FrameRenderer& renderer);
    static std::uint64_t temporalHistoryGeneration(
        const FrameRenderer& renderer);
    static bool temporalHistoryValid(const FrameRenderer& renderer);
    static void markTemporalHistoryValid(FrameRenderer& renderer);
};

} // namespace Rigel::Render
