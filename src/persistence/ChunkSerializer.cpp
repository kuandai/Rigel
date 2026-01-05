#include "Rigel/Persistence/ChunkSerializer.h"

#include <stdexcept>

namespace Rigel::Persistence {
namespace {

size_t spanVolume(const ChunkSpan& span) {
    return static_cast<size_t>(span.sizeX) *
        static_cast<size_t>(span.sizeY) *
        static_cast<size_t>(span.sizeZ);
}

void validateSpan(const ChunkSpan& span) {
    if (span.sizeX <= 0 || span.sizeY <= 0 || span.sizeZ <= 0) {
        throw std::runtime_error("ChunkSerializer: span size must be positive");
    }
    if (span.offsetX < 0 || span.offsetY < 0 || span.offsetZ < 0) {
        throw std::runtime_error("ChunkSerializer: span offset must be non-negative");
    }
    if (span.offsetX + span.sizeX > Voxel::Chunk::SIZE ||
        span.offsetY + span.sizeY > Voxel::Chunk::SIZE ||
        span.offsetZ + span.sizeZ > Voxel::Chunk::SIZE) {
        throw std::runtime_error("ChunkSerializer: span out of chunk bounds");
    }
}

}

ChunkData serializeChunk(const Voxel::Chunk& chunk) {
    ChunkSpan span;
    span.chunkX = chunk.position().x;
    span.chunkY = chunk.position().y;
    span.chunkZ = chunk.position().z;
    span.sizeX = Voxel::Chunk::SIZE;
    span.sizeY = Voxel::Chunk::SIZE;
    span.sizeZ = Voxel::Chunk::SIZE;
    return serializeChunkSpan(chunk, span);
}

ChunkData serializeChunkSpan(const Voxel::Chunk& chunk, const ChunkSpan& span) {
    validateSpan(span);

    ChunkData data;
    data.span = span;
    data.blocks.resize(spanVolume(span));

    for (int z = 0; z < span.sizeZ; ++z) {
        for (int y = 0; y < span.sizeY; ++y) {
            for (int x = 0; x < span.sizeX; ++x) {
                size_t index = static_cast<size_t>(x +
                    z * span.sizeX +
                    y * span.sizeX * span.sizeZ);
                int localX = span.offsetX + x;
                int localY = span.offsetY + y;
                int localZ = span.offsetZ + z;
                data.blocks[index] = chunk.getBlock(localX, localY, localZ);
            }
        }
    }

    return data;
}

void applyChunkData(const ChunkData& data, Voxel::Chunk& chunk, const Voxel::BlockRegistry& registry) {
    const ChunkSpan& span = data.span;
    validateSpan(span);

    size_t expected = spanVolume(span);
    if (data.blocks.size() != expected) {
        throw std::runtime_error("ChunkSerializer: block data size mismatch");
    }

    for (int z = 0; z < span.sizeZ; ++z) {
        for (int y = 0; y < span.sizeY; ++y) {
            for (int x = 0; x < span.sizeX; ++x) {
                size_t index = static_cast<size_t>(x +
                    z * span.sizeX +
                    y * span.sizeX * span.sizeZ);
                int localX = span.offsetX + x;
                int localY = span.offsetY + y;
                int localZ = span.offsetZ + z;
                chunk.setBlock(localX, localY, localZ, data.blocks[index], registry);
            }
        }
    }
}

} // namespace Rigel::Persistence
