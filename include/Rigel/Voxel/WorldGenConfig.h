#pragma once

#include "Chunk.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rigel::Voxel {

/**
 * @brief Configuration values for world generation and streaming.
 *
 * Loaded via a layered config provider and applied to WorldGenerator
 * and ChunkStreamer.
 */
struct WorldGenConfig {
    struct WorldConfig {
        int minY = -64;
        int maxY = 320;
        int seaLevel = 0;
        int lavaLevel = -32;
        uint32_t version = 1;
    };

    struct NoiseConfig {
        int octaves = 5;
        float frequency = 0.005f;
        float lacunarity = 2.0f;
        float persistence = 0.5f;
        float scale = 1.0f;
        float offset = 0.0f;
    };

    struct TerrainConfig {
        float baseHeight = 16.0f;
        float heightVariation = 16.0f;
        int surfaceDepth = 3;
        NoiseConfig heightNoise;
        NoiseConfig densityNoise;
        float densityStrength = 0.0f;
        float gradientStrength = 1.0f;
    };

    struct ClimateLayerConfig {
        NoiseConfig temperature;
        NoiseConfig humidity;
        NoiseConfig continentalness;
    };

    struct ClimateConfig {
        ClimateLayerConfig global;
        ClimateLayerConfig local;
        float localBlend = 1.0f;
        float latitudeScale = 0.0f;
        float latitudeStrength = 0.0f;
        float elevationLapse = 0.0f;
    };

    struct BiomeTarget {
        float temperature = 0.0f;
        float humidity = 0.0f;
        float continentalness = 0.0f;
    };

    struct SurfaceLayer {
        std::string block;
        int depth = 1;
    };

    struct BiomeConfig {
        std::string name;
        BiomeTarget target;
        float weight = 1.0f;
        std::vector<SurfaceLayer> surface;
    };

    struct BiomeBlendConfig {
        float blendPower = 2.0f;
        float epsilon = 0.0001f;
    };

    struct BiomesConfig {
        BiomeBlendConfig blend;
        std::vector<BiomeConfig> entries;
        struct CoastBandConfig {
            std::string biome;
            float minContinentalness = 0.0f;
            float maxContinentalness = 0.0f;
            bool enabled = false;
        } coastBand;
    };

    struct DensityNodeConfig {
        std::string id;
        std::string type;
        std::vector<std::string> inputs;
        std::string field;
        NoiseConfig noise;
        float value = 0.0f;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        float scale = 1.0f;
        float offset = 0.0f;
        std::vector<std::pair<float, float>> splinePoints;
    };

    struct DensityGraphConfig {
        std::vector<DensityNodeConfig> nodes;
        std::unordered_map<std::string, std::string> outputs;
    };

    struct CavesConfig {
        bool enabled = true;
        std::string densityOutput = "cave_density";
        float threshold = 0.5f;
        int sampleStep = 4;
    };

    struct FeatureConfig {
        std::string name;
        std::string block;
        float chance = 0.0f;
        int minHeight = 1;
        int maxHeight = 3;
        std::vector<std::string> biomes;
    };

    struct StructuresConfig {
        std::vector<FeatureConfig> features;
    };

    struct OverlayConfig {
        std::string path;
        std::string when;
    };

    struct StreamConfig {
        int viewDistanceChunks = 6;
        int unloadDistanceChunks = 8;
        size_t genQueueLimit = 0;
        size_t meshQueueLimit = 0;
        int applyBudgetPerFrame = 0;
        int workerThreads = 2;
        size_t maxResidentChunks = 0;  // 0 = no cap
    };

    struct PersistenceConfig {
        struct CRConfig {
            bool lz4 = false;
        } cr;
    };

    uint32_t seed = 1337;
    std::string solidBlock = "base:debug";
    std::string surfaceBlock = "base:debug";
    WorldConfig world;
    TerrainConfig terrain;
    ClimateConfig climate;
    BiomesConfig biomes;
    DensityGraphConfig densityGraph;
    CavesConfig caves;
    StructuresConfig structures;
    StreamConfig stream;
    PersistenceConfig persistence;

    // Stage enable flags keyed by stage name.
    std::unordered_map<std::string, bool> stageEnabled;
    std::unordered_map<std::string, bool> flags;
    std::vector<OverlayConfig> overlays;

    void applyYaml(const char* sourceName, const std::string& yaml);
    bool isStageEnabled(const std::string& stage) const;
    bool isFlagEnabled(const std::string& name) const;
};

} // namespace Rigel::Voxel
