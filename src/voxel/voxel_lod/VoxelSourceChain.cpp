#include "Rigel/Voxel/VoxelLod/VoxelSourceChain.h"

namespace Rigel::Voxel {

BrickSampleStatus VoxelSourceChain::sampleBrick(const BrickSampleDesc& desc,
                                                std::span<VoxelId> out,
                                                const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return BrickSampleStatus::Cancelled;
    }

    const size_t voxelCount = desc.outVoxelCount();

    auto trySource = [&](const IVoxelSource* source) -> BrickSampleStatus {
        if (!source) {
            return BrickSampleStatus::Miss;
        }
        return source->sampleBrick(desc, out, cancel);
    };

    BrickSampleStatus status = BrickSampleStatus::Miss;
    status = trySource(m_loaded);
    if (status == BrickSampleStatus::Hit) {
        ++m_telemetry.bricksSampled;
        m_telemetry.voxelsSampled += voxelCount;
        ++m_telemetry.loadedHits;
        return status;
    }
    if (status == BrickSampleStatus::Cancelled) {
        return status;
    }

    status = trySource(m_persistence);
    if (status == BrickSampleStatus::Hit) {
        ++m_telemetry.bricksSampled;
        m_telemetry.voxelsSampled += voxelCount;
        ++m_telemetry.persistenceHits;
        return status;
    }
    if (status == BrickSampleStatus::Cancelled) {
        return status;
    }

    status = trySource(m_generator);
    if (status == BrickSampleStatus::Hit) {
        ++m_telemetry.bricksSampled;
        m_telemetry.voxelsSampled += voxelCount;
        ++m_telemetry.generatorHits;
    }
    return status;
}

} // namespace Rigel::Voxel

