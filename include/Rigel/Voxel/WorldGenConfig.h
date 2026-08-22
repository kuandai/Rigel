#pragma once

#include "Chunk.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rigel::Voxel {

class WorldConfigProvider;

/**
 * @brief Configuration values for world generation.
 *
 * Loaded via a layered config provider and applied to WorldGenerator.
 */
struct WorldGenConfig {
    static constexpr int MinWorldY = -4096;
    static constexpr int MaxWorldY = 4096;
    static constexpr int MaxWorldHeight = 1024;
    static constexpr int MaxNoiseOctaves = 16;
    static constexpr int MaxSurfaceDepth = 32;
    static constexpr int MaxStructureHeight = MaxWorldHeight;
    static constexpr size_t MaxBiomeEntries = 32;
    static constexpr size_t MaxSurfaceLayers = 32;
    static constexpr size_t MaxStructureFeatures = 16;
    static constexpr size_t MaxFeatureBiomeFilters = 32;
    static constexpr size_t MaxDensityGraphNodes = 32;
    static constexpr size_t MaxDensityNodeInputs = 8;
    static constexpr size_t MaxDensitySplinePoints = 16;
    static constexpr size_t MaxDensityGraphOutputs = 8;

    struct WorldConfig {
        int minY = -64;
        int maxY = 320;
        int seaLevel = 0;
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
        std::string densityOutput = "cave_density";
        float threshold = 0.5f;
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

    // Stage enable flags keyed by stage name.
    std::unordered_map<std::string, bool> stageEnabled;
    std::unordered_map<std::string, bool> flags;
    std::vector<OverlayConfig> overlays;

    void applyYaml(const char* sourceName, const std::string& yaml);
    void validate(const char* sourceName) const;
    bool isStageEnabled(const std::string& stage) const;
    bool isFlagEnabled(const std::string& name) const;

private:
    friend class WorldConfigProvider;
    std::vector<OverlayConfig> applyYamlWithOverlays(
        const char* sourceName,
        const std::string& yaml
    );
};

} // namespace Rigel::Voxel
