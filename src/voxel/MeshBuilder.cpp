#include "Rigel/Voxel/MeshBuilder.h"

#include <algorithm>
#include <array>

namespace Rigel::Voxel {

namespace {

// Face vertex data for a unit cube
// Each face has 4 vertices in counter-clockwise order when viewed from outside
// Position offsets [vertex][xyz], UV coords [vertex][uv]

// Position offsets for each face (indexed by Direction, then vertex 0-3)
constexpr float FACE_POSITIONS[6][4][3] = {
    // PosX (+X, East)
    {{1, 0, 1}, {1, 1, 1}, {1, 1, 0}, {1, 0, 0}},
    // NegX (-X, West)
    {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}},
    // PosY (+Y, Up)
    {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
    // NegY (-Y, Down)
    {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
    // PosZ (+Z, South)
    {{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}},
    // NegZ (-Z, North)
    {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}}
};

// UV coordinates for each face (same for all faces currently)
constexpr float FACE_UVS[4][2] = {
    {0, 0}, {0, 1}, {1, 1}, {1, 0}
};

std::array<std::array<float, 2>, 4> modelFaceUvs(
    const BlockModelFace& face, size_t additionalQuarterTurns = 0
) {
    const std::array<std::array<float, 2>, 4> base = {{
        {face.uv.u0, face.uv.v0},
        {face.uv.u0, face.uv.v1},
        {face.uv.u1, face.uv.v1},
        {face.uv.u1, face.uv.v0},
    }};
    std::array<std::array<float, 2>, 4> rotated{};
    const size_t quarterTurns =
        (static_cast<size_t>(face.rotation) + additionalQuarterTurns) % 4;
    for (size_t vertex = 0; vertex < rotated.size(); ++vertex) {
        rotated[vertex] = base[(vertex + quarterTurns) % base.size()];
    }
    return rotated;
}

uint16_t textureLayer(
    const TextureAtlas* atlas, const std::string* texturePath
) {
    if (!atlas || !texturePath || texturePath->empty()) {
        return 0;
    }
    const TextureHandle handle = atlas->findTexture(*texturePath);
    return handle.isValid()
        ? static_cast<uint16_t>(atlas->getLayer(handle))
        : 0;
}

// Quad indices (two triangles per face)
constexpr std::array<uint32_t, 6> QUAD_INDICES = {0, 1, 2, 0, 2, 3};
constexpr std::array<uint32_t, 6> QUAD_INDICES_FLIPPED = {0, 1, 3, 1, 2, 3};

struct Axis {
    int x;
    int y;
    int z;
};

constexpr Axis FACE_NORMALS[6] = {
    { 1,  0,  0},  // PosX
    {-1,  0,  0},  // NegX
    { 0,  1,  0},  // PosY
    { 0, -1,  0},  // NegY
    { 0,  0,  1},  // PosZ
    { 0,  0, -1}   // NegZ
};

Direction orientedDirection(
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

BlockModelBounds orientedBounds(
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

std::array<int, 3> orientationQuarterTurns(
    BlockModelOrientation orientation
) {
    switch (orientation) {
        case BlockModelOrientation::Identity: return {0, 0, 0};
        case BlockModelOrientation::RotateX90: return {1, 0, 0};
        case BlockModelOrientation::RotateX270: return {3, 0, 0};
        case BlockModelOrientation::RotateY90: return {0, 1, 0};
        case BlockModelOrientation::RotateY180: return {0, 2, 0};
        case BlockModelOrientation::RotateY270: return {0, 3, 0};
        case BlockModelOrientation::RotateZ90: return {0, 0, 1};
    }
    return {0, 0, 0};
}

size_t orientedUvQuarterTurns(
    const BlockModelInstance& instance, Direction destination
) {
    // UV adjustment is part of the normalized orientation convention. It is
    // applied after remapping the face, with the top/bottom correction kept
    // as independent authored state rather than inferred from the turn.
    const auto [x, y, z] = orientationQuarterTurns(instance.orientation);
    int adjustment = 0;
    switch (destination) {
        case Direction::PosX: adjustment += x; break;
        case Direction::NegX: adjustment -= x; break;
        case Direction::PosY: adjustment += y; break;
        case Direction::NegY: adjustment -= y; break;
        case Direction::PosZ: adjustment += z; break;
        case Direction::NegZ:
            adjustment -= z;
            if (x == 1 || x == 3) adjustment += 2;
            break;
    }
    if (instance.rotateTopBottomUv && z != 0) {
        switch (destination) {
            case Direction::PosX:
            case Direction::PosY: adjustment += z; break;
            case Direction::NegX:
            case Direction::NegY: adjustment -= z; break;
            case Direction::PosZ:
            case Direction::NegZ: break;
        }
    }
    return static_cast<size_t>((adjustment % 4 + 4) % 4);
}

constexpr Axis FACE_U_AXES[6] = {
    {0, 1, 0},  // PosX
    {0, 1, 0},  // NegX
    {1, 0, 0},  // PosY
    {1, 0, 0},  // NegY
    {1, 0, 0},  // PosZ
    {1, 0, 0}   // NegZ
};

constexpr Axis FACE_V_AXES[6] = {
    {0, 0, 1},  // PosX
    {0, 0, 1},  // NegX
    {0, 0, 1},  // PosY
    {0, 0, 1},  // NegY
    {0, 1, 0},  // PosZ
    {0, 1, 0}   // NegZ
};

int axisSign(const float pos[3], const Axis& axis) {
    if (axis.x != 0) {
        return (pos[0] > 0.5f) ? 1 : -1;
    }
    if (axis.y != 0) {
        return (pos[1] > 0.5f) ? 1 : -1;
    }
    return (pos[2] > 0.5f) ? 1 : -1;
}

bool isFullCellOccluder(const BlockType& type) {
    if (!type.isOpaque || !type.model) {
        return false;
    }
    if (type.model->isFullCube()) {
        return true;
    }
    for (const BlockModelCuboid& cuboid : type.model->cuboids()) {
        const bool hasClosedSurface = std::all_of(
            cuboid.faces.begin(), cuboid.faces.end(),
            [](const auto& face) { return face.has_value(); });
        if (!hasClosedSurface) {
            continue;
        }
        bool coversCell = true;
        for (size_t axis = 0; axis < 3; ++axis) {
            if (cuboid.bounds.min[axis] > 0.0f ||
                cuboid.bounds.max[axis] < 1.0f) {
                coversCell = false;
                break;
            }
        }
        if (coversCell) {
            return true;
        }
    }
    return false;
}

bool isOccluder(const BlockState& state, const BlockRegistry& registry) {
    return !state.isAir() &&
        isFullCellOccluder(registry.getType(state.id));
}

bool isCellBoundaryFace(
    const BlockModelBounds& bounds, Direction direction
) {
    const size_t faceIndex = static_cast<size_t>(direction);
    const size_t normalAxis = faceIndex / 2;
    return faceIndex % 2 == 0
        ? bounds.max[normalAxis] == 1.0f
        : bounds.min[normalAxis] == 0.0f;
}

bool hasMatchingOppositeBoundaryFace(
    const BlockModelInstance& instance,
    const BlockModelBounds& bounds,
    Direction direction
) {
    if (!isCellBoundaryFace(bounds, direction)) {
        return false;
    }

    const Direction oppositeDirection = opposite(direction);
    const size_t normalAxis = static_cast<size_t>(direction) / 2;
    for (const BlockModelCuboid& candidate : instance->cuboids()) {
        const BlockModelBounds candidateBounds = orientedBounds(
            candidate.bounds, instance.orientation);
        if (!isCellBoundaryFace(candidateBounds, oppositeDirection)) {
            continue;
        }

        bool matchingRectangle = true;
        for (size_t axis = 0; axis < 3; ++axis) {
            if (axis != normalAxis &&
                (candidateBounds.min[axis] != bounds.min[axis] ||
                 candidateBounds.max[axis] != bounds.max[axis])) {
                matchingRectangle = false;
                break;
            }
        }
        if (!matchingRectangle) {
            continue;
        }

        for (size_t sourceFaceIdx = 0;
             sourceFaceIdx < DirectionCount; ++sourceFaceIdx) {
            if (candidate.faces[sourceFaceIdx] &&
                orientedDirection(
                    static_cast<Direction>(sourceFaceIdx),
                    instance.orientation) == oppositeDirection) {
                return true;
            }
        }
    }
    return false;
}

bool isCoveredByFullCellNeighbor(
    const BlockModelBounds& bounds, Direction direction
) {
    if (!isCellBoundaryFace(bounds, direction)) {
        return false;
    }
    const size_t normalAxis = static_cast<size_t>(direction) / 2;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (axis != normalAxis &&
            (bounds.min[axis] < 0.0f || bounds.max[axis] > 1.0f)) {
            return false;
        }
    }
    return true;
}

bool isCubeAoCompatibleFace(
    const BlockModelBounds& bounds, Direction direction
) {
    if (!isCellBoundaryFace(bounds, direction)) {
        return false;
    }
    const size_t normalAxis = static_cast<size_t>(direction) / 2;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (axis != normalAxis &&
            (bounds.min[axis] != 0.0f || bounds.max[axis] != 1.0f)) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

ChunkMesh MeshBuilder::build(const BuildContext& ctx) const {
    ChunkMesh mesh;

    // Skip empty chunks
    if (ctx.chunk.isEmpty()) {
        return mesh;
    }

    // Reserve estimated capacity
    // Rough estimate: average visible blocks * faces * vertices
    const size_t estimatedBlocks = ctx.chunk.nonAirCount();
    mesh.vertices.reserve(estimatedBlocks * 6);  // ~1 face per block average
    mesh.indices.reserve(estimatedBlocks * 6);

    // Temporary storage for each layer
    std::array<std::vector<VoxelVertex>, RenderLayerCount> layerVertices;
    std::array<std::vector<uint32_t>, RenderLayerCount> layerIndices;

    for (int z = 0; z < Chunk::SIZE; z++) {
        for (int y = 0; y < Chunk::SIZE; y++) {
            for (int x = 0; x < Chunk::SIZE; x++) {
                BlockState state = ctx.chunk.getBlock(x, y, z);

                // Skip air
                if (state.isAir()) {
                    continue;
                }

                const BlockType& type = ctx.registry.getType(state.id);

                if (!type.model) {
                    continue;
                }

                // Get layer for this block type
                size_t layerIdx = static_cast<size_t>(type.layer);

                if (type.model->isFullCube()) {
                    // Keep the canonical cube on its specialized path.
                    for (size_t sourceFaceIdx = 0;
                         sourceFaceIdx < DirectionCount; ++sourceFaceIdx) {
                        const Direction sourceFace =
                            static_cast<Direction>(sourceFaceIdx);
                        const Direction face = orientedDirection(
                            sourceFace, type.model.orientation);
                        const size_t faceIdx = static_cast<size_t>(face);

                        if (!shouldRenderFace(ctx, x, y, z, face, state, type)) {
                            continue;
                        }

                        std::array<uint8_t, 4> aoLevels{};
                        for (size_t v = 0; v < 4; ++v) {
                            aoLevels[v] = calculateAO(
                                ctx, x, y, z, face, static_cast<int>(v));
                        }

                        uint32_t baseVertex = static_cast<uint32_t>(
                            layerVertices[layerIdx].size());
                        const uint16_t faceTextureLayer = textureLayer(
                            ctx.atlas, &type.textures.forFace(sourceFace));
                        const size_t uvQuarterTurns = orientedUvQuarterTurns(
                            type.model, face);
                        const auto& modelFace = *type.model->cuboids().front()
                            .faces[sourceFaceIdx];
                        const auto orientedUvs = modelFaceUvs(
                            modelFace, uvQuarterTurns);

                        for (size_t v = 0; v < 4; v++) {
                            VoxelVertex vertex;
                            vertex.x = static_cast<float>(x) + FACE_POSITIONS[faceIdx][v][0];
                            vertex.y = static_cast<float>(y) + FACE_POSITIONS[faceIdx][v][1];
                            vertex.z = static_cast<float>(z) + FACE_POSITIONS[faceIdx][v][2];
                            vertex.u = type.model.orientation ==
                                    BlockModelOrientation::Identity
                                ? FACE_UVS[v][0]
                                : orientedUvs[v][0];
                            vertex.v = type.model.orientation ==
                                    BlockModelOrientation::Identity
                                ? FACE_UVS[v][1]
                                : orientedUvs[v][1];
                            vertex.normalIndex = static_cast<uint8_t>(faceIdx);
                            vertex.aoLevel = aoLevels[v];
                            vertex.textureLayer = faceTextureLayer;

                            layerVertices[layerIdx].push_back(vertex);
                        }

                        bool flipDiagonal = (aoLevels[0] + aoLevels[2]) >
                            (aoLevels[1] + aoLevels[3]);
                        const auto& indices = flipDiagonal
                            ? QUAD_INDICES_FLIPPED
                            : QUAD_INDICES;
                        for (uint32_t idx : indices) {
                            layerIndices[layerIdx].push_back(baseVertex + idx);
                        }
                    }
                    continue;
                }

                for (const BlockModelCuboid& cuboid : type.model->cuboids()) {
                    const BlockModelBounds bounds = orientedBounds(
                        cuboid.bounds, type.model.orientation);
                    for (size_t sourceFaceIdx = 0;
                         sourceFaceIdx < DirectionCount; ++sourceFaceIdx) {
                        const auto& optionalFace = cuboid.faces[sourceFaceIdx];
                        if (!optionalFace) {
                            continue;
                        }
                        const Direction sourceDirection =
                            static_cast<Direction>(sourceFaceIdx);
                        const Direction direction =
                            orientedDirection(
                                sourceDirection, type.model.orientation);
                        const Direction shadingDirection = orientedDirection(
                            optionalFace->shadingFace.value_or(sourceDirection),
                            type.model.orientation);
                        const size_t faceIdx = static_cast<size_t>(direction);
                        const BlockModelFace& face = *optionalFace;
                        const bool coveredByFullNeighbor =
                            isCoveredByFullCellNeighbor(
                                bounds, direction);
                        if (!shouldRenderFace(
                                ctx, x, y, z, direction, state, type,
                                face.cullAgainstOpaqueNeighbor &&
                                    coveredByFullNeighbor,
                                type.cullSameType &&
                                    hasMatchingOppositeBoundaryFace(
                                        type.model, bounds, direction))) {
                            continue;
                        }

                        std::array<uint8_t, 4> aoLevels{};
                        aoLevels.fill(3);
                        if (face.ambientOcclusion &&
                            isCubeAoCompatibleFace(bounds, direction)) {
                            for (size_t vertex = 0; vertex < aoLevels.size(); ++vertex) {
                                aoLevels[vertex] = calculateAO(
                                    ctx, x, y, z, direction,
                                    static_cast<int>(vertex));
                            }
                        }

                        const uint32_t baseVertex = static_cast<uint32_t>(
                            layerVertices[layerIdx].size());
                        const std::array<std::array<float, 2>, 4> uvs =
                            modelFaceUvs(
                                face, orientedUvQuarterTurns(
                                    type.model, direction));
                        const uint16_t faceTextureLayer = textureLayer(
                            ctx.atlas, type.textures.find(face.textureSlot));

                        for (size_t vertexIndex = 0; vertexIndex < 4;
                             ++vertexIndex) {
                            const float* unitPosition =
                                FACE_POSITIONS[faceIdx][vertexIndex];
                            VoxelVertex vertex;
                            vertex.x = static_cast<float>(x) +
                                (unitPosition[0] == 0.0f
                                     ? bounds.min[0]
                                     : bounds.max[0]);
                            vertex.y = static_cast<float>(y) +
                                (unitPosition[1] == 0.0f
                                     ? bounds.min[1]
                                     : bounds.max[1]);
                            vertex.z = static_cast<float>(z) +
                                (unitPosition[2] == 0.0f
                                     ? bounds.min[2]
                                     : bounds.max[2]);
                            vertex.u = uvs[vertexIndex][0];
                            vertex.v = uvs[vertexIndex][1];
                            vertex.normalIndex = static_cast<uint8_t>(
                                shadingDirection);
                            vertex.aoLevel = aoLevels[vertexIndex];
                            vertex.textureLayer = faceTextureLayer;
                            layerVertices[layerIdx].push_back(vertex);
                        }

                        const bool flipDiagonal =
                            (aoLevels[0] + aoLevels[2]) >
                            (aoLevels[1] + aoLevels[3]);
                        const auto& indices = flipDiagonal
                            ? QUAD_INDICES_FLIPPED
                            : QUAD_INDICES;
                        for (const uint32_t index : indices) {
                            layerIndices[layerIdx].push_back(baseVertex + index);
                        }
                    }
                }
            }
        }
    }

    // Combine layers into final mesh
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;

    for (size_t layer = 0; layer < RenderLayerCount; layer++) {
        mesh.layers[layer].indexStart = indexOffset;
        mesh.layers[layer].indexCount = static_cast<uint32_t>(layerIndices[layer].size());

        // Append vertices
        for (const auto& v : layerVertices[layer]) {
            mesh.vertices.push_back(v);
        }

        // Append indices (adjusted by vertex offset)
        for (uint32_t idx : layerIndices[layer]) {
            mesh.indices.push_back(idx + vertexOffset);
        }

        vertexOffset += static_cast<uint32_t>(layerVertices[layer].size());
        indexOffset += static_cast<uint32_t>(layerIndices[layer].size());
    }

    return mesh;
}

bool MeshBuilder::shouldRenderFace(
    const BuildContext& ctx,
    int x, int y, int z,
    Direction face,
    const BlockState& state,
    const BlockType& type,
    bool cullAgainstOpaqueNeighbor,
    bool cullSameTypeNeighbor
) const {
    // Get offset for this direction
    int dx, dy, dz;
    directionOffset(face, dx, dy, dz);

    // Get neighbor block
    BlockState neighbor = getBlockAt(ctx, x + dx, y + dy, z + dz);

    // Render face if neighbor is air
    if (neighbor.isAir()) {
        return true;
    }

    const BlockType& neighborType = ctx.registry.getType(neighbor.id);
    if (cullAgainstOpaqueNeighbor && isFullCellOccluder(neighborType)) {
        return false;
    }

    if (cullSameTypeNeighbor && type.cullSameType && neighbor.id == state.id) {
        return false;
    }

    return true;
}

BlockState MeshBuilder::getBlockAt(
    const BuildContext& ctx,
    int x, int y, int z
) const {
    if (ctx.paddedBlocks) {
        if (x < -1 || x > Chunk::SIZE ||
            y < -1 || y > Chunk::SIZE ||
            z < -1 || z > Chunk::SIZE) {
            return BlockState{};
        }

        int px = x + 1;
        int py = y + 1;
        int pz = z + 1;
        size_t index = static_cast<size_t>(px)
            + static_cast<size_t>(py) * MeshBuilder::PaddedSize
            + static_cast<size_t>(pz) * MeshBuilder::PaddedSize * MeshBuilder::PaddedSize;
        return (*ctx.paddedBlocks)[index];
    }

    // Inside current chunk
    if (x >= 0 && x < Chunk::SIZE &&
        y >= 0 && y < Chunk::SIZE &&
        z >= 0 && z < Chunk::SIZE) {
        return ctx.chunk.getBlock(x, y, z);
    }

    // Determine which neighbor chunk and local coordinates
    Direction dir;
    int nx = x, ny = y, nz = z;

    if (x < 0) {
        dir = Direction::NegX;
        nx = x + Chunk::SIZE;
    } else if (x >= Chunk::SIZE) {
        dir = Direction::PosX;
        nx = x - Chunk::SIZE;
    } else if (y < 0) {
        dir = Direction::NegY;
        ny = y + Chunk::SIZE;
    } else if (y >= Chunk::SIZE) {
        dir = Direction::PosY;
        ny = y - Chunk::SIZE;
    } else if (z < 0) {
        dir = Direction::NegZ;
        nz = z + Chunk::SIZE;
    } else {
        dir = Direction::PosZ;
        nz = z - Chunk::SIZE;
    }

    const Chunk* neighbor = ctx.neighbors[static_cast<size_t>(dir)];
    if (!neighbor) {
        // Missing neighbors expose the chunk boundary.
        return BlockState{};
    }

    return neighbor->getBlock(nx, ny, nz);
}

uint8_t MeshBuilder::calculateAO(
    const BuildContext& ctx,
    int x, int y, int z,
    Direction face,
    int corner
) const {
    const int faceIdx = static_cast<int>(face);
    const Axis& normal = FACE_NORMALS[faceIdx];
    const Axis& uAxis = FACE_U_AXES[faceIdx];
    const Axis& vAxis = FACE_V_AXES[faceIdx];

    const float* cornerPos = FACE_POSITIONS[faceIdx][corner];
    int uSign = axisSign(cornerPos, uAxis);
    int vSign = axisSign(cornerPos, vAxis);

    int ux = uAxis.x * uSign;
    int uy = uAxis.y * uSign;
    int uz = uAxis.z * uSign;
    int vx = vAxis.x * vSign;
    int vy = vAxis.y * vSign;
    int vz = vAxis.z * vSign;

    BlockState side1 = getBlockAt(
        ctx,
        x + normal.x + ux,
        y + normal.y + uy,
        z + normal.z + uz
    );
    BlockState side2 = getBlockAt(
        ctx,
        x + normal.x + vx,
        y + normal.y + vy,
        z + normal.z + vz
    );
    BlockState cornerBlock = getBlockAt(
        ctx,
        x + normal.x + ux + vx,
        y + normal.y + uy + vy,
        z + normal.z + uz + vz
    );

    bool side1Occ = isOccluder(side1, ctx.registry);
    bool side2Occ = isOccluder(side2, ctx.registry);
    bool cornerOcc = isOccluder(cornerBlock, ctx.registry);

    int occlusion = 0;
    if (side1Occ && side2Occ) {
        occlusion = 3;
    } else {
        occlusion = static_cast<int>(side1Occ) +
            static_cast<int>(side2Occ) +
            static_cast<int>(cornerOcc);
    }

    return static_cast<uint8_t>(3 - occlusion);
}

} // namespace Rigel::Voxel
