#pragma once

#include "Chunk.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Rigel::Voxel {

/**
 * @brief Configuration values for world generation and streaming.
 *
 * Loaded via a layered config provider and applied to WorldGenerator
 * and ChunkStreamer.
 */
struct WorldGenConfig {
    struct NoiseConfig {
        int octaves = 5;
        float frequency = 0.005f;
        float lacunarity = 2.0f;
        float persistence = 0.5f;
    };

    struct TerrainConfig {
        float baseHeight = 16.0f;
        float heightVariation = 16.0f;
        int surfaceDepth = 3;
        NoiseConfig heightNoise;
    };

    struct StreamConfig {
        int viewDistanceChunks = 6;
        int unloadDistanceChunks = 8;
        int maxGeneratePerFrame = 2;
        size_t maxResidentChunks = 0;  // 0 = no cap
    };

    uint32_t seed = 1337;
    std::string solidBlock = "rigel:debug_block";
    std::string surfaceBlock = "rigel:debug_block";
    TerrainConfig terrain;
    StreamConfig stream;

    // Stage enable flags keyed by stage name.
    std::unordered_map<std::string, bool> stageEnabled;

    void applyYaml(const char* sourceName, const std::string& yaml);
    bool isStageEnabled(const std::string& stage) const;
};

} // namespace Rigel::Voxel
