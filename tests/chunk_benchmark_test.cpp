#include "TestFramework.h"

#include "Rigel/Voxel/ChunkBenchmark.h"

using namespace Rigel::Voxel;

TEST_CASE(ChunkBenchmarkStats_Counts) {
    ChunkBenchmarkStats stats;
    stats.addGeneration(0.1);
    stats.addGeneration(0.2);
    stats.addMesh(0.3, false);
    stats.addMesh(0.4, true);

    CHECK_EQ(stats.generatedChunks, static_cast<uint64_t>(2));
    CHECK_EQ(stats.meshedChunks, static_cast<uint64_t>(1));
    CHECK_EQ(stats.emptyChunks, static_cast<uint64_t>(1));
    CHECK_EQ(stats.processedChunks(), static_cast<uint64_t>(2));
    CHECK_NEAR(stats.generationSeconds, 0.3, 0.0001);
    CHECK_NEAR(stats.meshSeconds, 0.3, 0.0001);
    CHECK_NEAR(stats.emptyMeshSeconds, 0.4, 0.0001);
}
