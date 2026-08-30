#pragma once

#include "Rigel/Voxel/BlockType.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace Rigel::Voxel::detail {

inline Direction orientedDirection(
    Direction source, BlockModelOrientation orientation
) {
    constexpr std::array<Direction, DirectionCount> identity = {
        Direction::PosX, Direction::NegX, Direction::PosY,
        Direction::NegY, Direction::PosZ, Direction::NegZ};
    constexpr std::array<Direction, DirectionCount> rotateX90 = {
        Direction::PosX, Direction::NegX, Direction::NegZ,
        Direction::PosZ, Direction::PosY, Direction::NegY};
    constexpr std::array<Direction, DirectionCount> rotateX270 = {
        Direction::PosX, Direction::NegX, Direction::PosZ,
        Direction::NegZ, Direction::NegY, Direction::PosY};
    constexpr std::array<Direction, DirectionCount> rotateY90 = {
        Direction::PosZ, Direction::NegZ, Direction::PosY,
        Direction::NegY, Direction::NegX, Direction::PosX};
    constexpr std::array<Direction, DirectionCount> rotateY180 = {
        Direction::NegX, Direction::PosX, Direction::PosY,
        Direction::NegY, Direction::NegZ, Direction::PosZ};
    constexpr std::array<Direction, DirectionCount> rotateY270 = {
        Direction::NegZ, Direction::PosZ, Direction::PosY,
        Direction::NegY, Direction::PosX, Direction::NegX};
    constexpr std::array<Direction, DirectionCount> rotateZ90 = {
        Direction::NegY, Direction::PosY, Direction::PosX,
        Direction::NegX, Direction::PosZ, Direction::NegZ};

    const auto& directions = [&]() -> const auto& {
        switch (orientation) {
            case BlockModelOrientation::Identity: return identity;
            case BlockModelOrientation::RotateX90: return rotateX90;
            case BlockModelOrientation::RotateX270: return rotateX270;
            case BlockModelOrientation::RotateY90: return rotateY90;
            case BlockModelOrientation::RotateY180: return rotateY180;
            case BlockModelOrientation::RotateY270: return rotateY270;
            case BlockModelOrientation::RotateZ90: return rotateZ90;
        }
        return identity;
    }();
    return directions[static_cast<size_t>(source)];
}

inline BlockModelBounds orientedBounds(
    const BlockModelBounds& source, BlockModelOrientation orientation
) {
    const auto& min = source.min;
    const auto& max = source.max;
    switch (orientation) {
        case BlockModelOrientation::Identity:
            return source;
        case BlockModelOrientation::RotateX90:
            return {{min[0], min[2], 1.0f - max[1]},
                    {max[0], max[2], 1.0f - min[1]}};
        case BlockModelOrientation::RotateX270:
            return {{min[0], 1.0f - max[2], min[1]},
                    {max[0], 1.0f - min[2], max[1]}};
        case BlockModelOrientation::RotateY90:
            return {{1.0f - max[2], min[1], min[0]},
                    {1.0f - min[2], max[1], max[0]}};
        case BlockModelOrientation::RotateY180:
            return {{1.0f - max[0], min[1], 1.0f - max[2]},
                    {1.0f - min[0], max[1], 1.0f - min[2]}};
        case BlockModelOrientation::RotateY270:
            return {{min[2], min[1], 1.0f - max[0]},
                    {max[2], max[1], 1.0f - min[0]}};
        case BlockModelOrientation::RotateZ90:
            return {{min[1], 1.0f - max[0], min[2]},
                    {max[1], 1.0f - min[0], max[2]}};
    }
    return source;
}

inline bool isCellBoundaryFace(
    const BlockModelBounds& bounds, Direction direction
) {
    const size_t faceIndex = static_cast<size_t>(direction);
    const size_t normalAxis = faceIndex / 2;
    return faceIndex % 2 == 0
        ? bounds.max[normalAxis] == 1.0f
        : bounds.min[normalAxis] == 0.0f;
}

struct BoundaryRectangle {
    float minU;
    float maxU;
    float minV;
    float maxV;
};

inline std::optional<BoundaryRectangle> boundaryRectangle(
    const BlockModelBounds& bounds,
    Direction direction,
    bool requireInsideCell
) {
    if (!isCellBoundaryFace(bounds, direction)) {
        return std::nullopt;
    }

    std::array<size_t, 2> tangentAxes{};
    const size_t normalAxis = static_cast<size_t>(direction) / 2;
    size_t tangent = 0;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (axis != normalAxis) {
            tangentAxes[tangent++] = axis;
        }
    }

    const BoundaryRectangle rectangle{
        bounds.min[tangentAxes[0]], bounds.max[tangentAxes[0]],
        bounds.min[tangentAxes[1]], bounds.max[tangentAxes[1]]};
    if (!(rectangle.minU < rectangle.maxU) ||
        !(rectangle.minV < rectangle.maxV)) {
        return std::nullopt;
    }
    if (requireInsideCell &&
        (rectangle.minU < 0.0f || rectangle.maxU > 1.0f ||
         rectangle.minV < 0.0f || rectangle.maxV > 1.0f)) {
        return std::nullopt;
    }
    return rectangle;
}

inline bool rectanglesCover(
    const BoundaryRectangle& source,
    std::vector<BoundaryRectangle> candidates
) {
    std::sort(
        candidates.begin(), candidates.end(),
        [](const BoundaryRectangle& left,
           const BoundaryRectangle& right) {
            if (left.minV != right.minV) return left.minV < right.minV;
            return left.maxV < right.maxV;
        });

    std::vector<float> uEdges = {source.minU, source.maxU};
    for (const BoundaryRectangle& candidate : candidates) {
        if (candidate.maxU <= source.minU ||
            candidate.minU >= source.maxU ||
            candidate.maxV <= source.minV ||
            candidate.minV >= source.maxV) {
            continue;
        }
        if (candidate.minU > source.minU && candidate.minU < source.maxU) {
            uEdges.push_back(candidate.minU);
        }
        if (candidate.maxU > source.minU && candidate.maxU < source.maxU) {
            uEdges.push_back(candidate.maxU);
        }
    }
    std::sort(uEdges.begin(), uEdges.end());
    uEdges.erase(std::unique(uEdges.begin(), uEdges.end()), uEdges.end());

    for (size_t edge = 1; edge < uEdges.size(); ++edge) {
        const float bandMin = uEdges[edge - 1];
        const float bandMax = uEdges[edge];
        float coveredThrough = source.minV;

        for (const BoundaryRectangle& candidate : candidates) {
            if (candidate.minU > bandMin || candidate.maxU < bandMax ||
                candidate.maxV <= coveredThrough ||
                candidate.minV >= source.maxV) {
                continue;
            }
            if (candidate.minV > coveredThrough) {
                break;
            }
            coveredThrough = candidate.maxV;
            if (coveredThrough >= source.maxV) {
                break;
            }
        }
        if (coveredThrough < source.maxV) {
            return false;
        }
    }
    return true;
}

inline bool oppositeBoundaryCovers(
    const BlockModelInstance& neighborModel,
    const BlockModelBounds& sourceBounds,
    Direction sourceDirection
) {
    const auto source = boundaryRectangle(
        sourceBounds, sourceDirection, true);
    if (!source || !neighborModel) {
        return false;
    }
    if (neighborModel->isFullCube()) {
        return true;
    }

    const Direction candidateDirection = opposite(sourceDirection);
    std::vector<BoundaryRectangle> candidates;
    for (const BlockModelCuboid& cuboid : neighborModel->cuboids()) {
        bool hasCandidateFace = false;
        for (size_t sourceFace = 0;
             sourceFace < DirectionCount; ++sourceFace) {
            if (cuboid.faces[sourceFace] &&
                orientedDirection(
                    static_cast<Direction>(sourceFace),
                    neighborModel.orientation) == candidateDirection) {
                hasCandidateFace = true;
                break;
            }
        }
        if (!hasCandidateFace) {
            continue;
        }

        const BlockModelBounds bounds = orientedBounds(
            cuboid.bounds, neighborModel.orientation);
        if (const auto rectangle = boundaryRectangle(
                bounds, candidateDirection, false)) {
            if (rectangle->minU <= source->minU &&
                rectangle->maxU >= source->maxU &&
                rectangle->minV <= source->minV &&
                rectangle->maxV >= source->maxV) {
                return true;
            }
            candidates.push_back(*rectangle);
        }
    }
    return rectanglesCover(*source, std::move(candidates));
}

enum class BoundaryCullReason {
    OpaqueCoverage,
    SameType,
};

inline bool identicalPairCullsBoundary(
    const BlockType& type,
    Direction direction,
    BoundaryCullReason reason
) {
    const bool sameTypeCulling = reason == BoundaryCullReason::SameType;
    if (!type.model || (sameTypeCulling
            ? !type.cullSameType
            : !type.isOpaque)) {
        return false;
    }
    for (const BlockModelCuboid& cuboid : type.model->cuboids()) {
        const BlockModelBounds bounds = orientedBounds(
            cuboid.bounds, type.model.orientation);
        for (size_t sourceFace = 0;
             sourceFace < DirectionCount; ++sourceFace) {
            const auto& face = cuboid.faces[sourceFace];
            if (!face ||
                orientedDirection(
                    static_cast<Direction>(sourceFace),
                    type.model.orientation) != direction ||
                (!sameTypeCulling && !face->cullAgainstOpaqueNeighbor)) {
                continue;
            }
            if (oppositeBoundaryCovers(type.model, bounds, direction)) {
                return true;
            }
        }
    }
    return false;
}

inline bool identicalPairCullsBothXBoundaries(
    const BlockType& type, BoundaryCullReason reason
) {
    return identicalPairCullsBoundary(
               type, Direction::PosX, reason) &&
        identicalPairCullsBoundary(
               type, Direction::NegX, reason);
}

} // namespace Rigel::Voxel::detail
