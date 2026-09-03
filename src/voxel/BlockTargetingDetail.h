#pragma once

#include "Rigel/Voxel/BlockTargeting.h"

#include <cstdint>
#include <optional>

namespace Rigel::Voxel {
class World;

namespace detail {

/** Work counters for deterministic raycast benchmark context. */
struct BlockRaycastCounters {
    uint64_t ddaCellsVisited = 0;
    uint64_t ownerCandidateSlots = 0;
    uint64_t ownerCandidateRetestsAvoided = 0;
    uint64_t ownersTested = 0;
    uint64_t nonAirOwnersTested = 0;
    uint64_t canonicalCubeTests = 0;
    uint64_t cuboidsTested = 0;
    uint64_t declaredFacesTested = 0;
};

/** Instrumented private entry point; gameplay uses the counter-free overload. */
std::optional<BlockTarget> raycastBlockWithCounters(
    const World& world,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance,
    BlockRaycastCounters& counters);

} // namespace detail
} // namespace Rigel::Voxel
