#pragma once

#include "Rigel/Voxel/RenderConfig.h"

#include <glm/vec3.hpp>
#include <cstdint>

namespace Rigel::Voxel {

struct SvoLodTelemetry {
    uint32_t activeCells = 0;
    uint32_t pendingCopies = 0;
    uint32_t pendingApplies = 0;
    uint64_t copiedCells = 0;
    uint64_t appliedCells = 0;
    uint64_t updateCalls = 0;
};

class SvoLodManager {
public:
    void setConfig(const SvoLodConfig& config);
    const SvoLodConfig& config() const { return m_config; }

    void initialize();
    void update(const glm::vec3& cameraPos);
    void reset();
    void releaseRenderResources();

    const SvoLodTelemetry& telemetry() const { return m_telemetry; }

private:
    static SvoLodConfig sanitizeConfig(SvoLodConfig config);

    SvoLodConfig m_config;
    SvoLodTelemetry m_telemetry;
};

} // namespace Rigel::Voxel
