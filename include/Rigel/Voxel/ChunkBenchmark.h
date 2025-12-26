#pragma once

#include <cstdint>

namespace Rigel::Voxel {

struct ChunkBenchmarkStats {
    uint64_t generatedChunks = 0;
    uint64_t meshedChunks = 0;
    uint64_t emptyChunks = 0;
    double generationSeconds = 0.0;
    double meshSeconds = 0.0;
    double emptyMeshSeconds = 0.0;

    void addGeneration(double seconds) {
        generationSeconds += seconds;
        ++generatedChunks;
    }

    void addMesh(double seconds, bool empty) {
        if (empty) {
            emptyMeshSeconds += seconds;
            ++emptyChunks;
        } else {
            meshSeconds += seconds;
            ++meshedChunks;
        }
    }

    uint64_t processedChunks() const {
        return meshedChunks + emptyChunks;
    }
};

} // namespace Rigel::Voxel
