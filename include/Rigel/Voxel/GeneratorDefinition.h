#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rigel::Voxel {

class BlockRegistry;

inline constexpr uint32_t kGeneratorDefinitionSchemaVersion = 2;

enum class GeneratorDefinitionOrigin {
    Shipped,
    ThirdParty
};

struct GeneratorDefinitionData {
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

    struct Bounds {
        int minY = 0;
        int maxY = 0;

        bool operator==(const Bounds&) const = default;
    } bounds;

    struct Terrain {
        int seaLevel = 0;
        std::string solidMaterial;
        std::string waterMaterial;
        std::string densityOutput;

        bool operator==(const Terrain&) const = default;
    } terrain;

    struct Noise {
        int octaves = 0;
        float frequency = 0.0f;
        float lacunarity = 0.0f;
        float persistence = 0.0f;
        float scale = 0.0f;
        float offset = 0.0f;

        bool operator==(const Noise&) const = default;
    };

    struct ClimateLayer {
        Noise temperature;
        Noise humidity;
        Noise continentalness;

        bool operator==(const ClimateLayer&) const = default;
    };

    struct Climate {
        float latitudeScale = 0.0f;
        float latitudeStrength = 0.0f;
        float localBlend = 0.0f;
        ClimateLayer global;
        ClimateLayer local;

        bool operator==(const Climate&) const = default;
    } climate;

    struct SurfaceLayer {
        std::string material;
        int depth = 0;

        bool operator==(const SurfaceLayer&) const = default;
    };

    struct BiomeTarget {
        float temperature = 0.0f;
        float humidity = 0.0f;
        float continentalness = 0.0f;

        bool operator==(const BiomeTarget&) const = default;
    };

    struct Biome {
        std::string id;
        BiomeTarget target;
        float weight = 0.0f;
        bool waterFill = false;
        std::vector<SurfaceLayer> surface;

        bool operator==(const Biome&) const = default;
    };

    struct Coast {
        std::string biome;
        float minContinentalness = 0.0f;
        float maxContinentalness = 0.0f;

        bool operator==(const Coast&) const = default;
    };

    struct Biomes {
        float blendPower = 0.0f;
        float epsilon = 0.0f;
        Coast coast;
        std::vector<Biome> entries;

        bool operator==(const Biomes&) const = default;
    } biomes;

    struct DensityNode {
        std::string id;
        std::string type;
        std::vector<std::string> inputs;
        std::string field;
        Noise noise;
        float value = 0.0f;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        float scale = 0.0f;
        float offset = 0.0f;
        std::vector<std::pair<float, float>> splinePoints;

        bool operator==(const DensityNode&) const = default;
    };

    struct DensityOutput {
        std::string semantic;
        std::string node;

        bool operator==(const DensityOutput&) const = default;
    };

    struct DensityGraph {
        std::vector<DensityOutput> outputs;
        std::vector<DensityNode> nodes;

        bool operator==(const DensityGraph&) const = default;
    } densityGraph;

    struct Caves {
        bool enabled = false;
        std::string densityOutput;
        float threshold = 0.0f;

        bool operator==(const Caves&) const = default;
    } caves;

    struct StructureFeature {
        std::string id;
        std::string material;
        float chance = 0.0f;
        int minHeight = 0;
        int maxHeight = 0;
        std::vector<std::string> biomes;

        bool operator==(const StructureFeature&) const = default;
    };

    struct Structures {
        bool enabled = false;
        std::vector<StructureFeature> features;

        bool operator==(const Structures&) const = default;
    } structures;

    bool operator==(const GeneratorDefinitionData&) const = default;
};

struct GeneratorDefinition {
    uint32_t schemaVersion = 0;
    std::string id;
    uint32_t sourceRevision = 0;
    std::string label;
    std::string description;
    GeneratorDefinitionData data;

    bool operator==(const GeneratorDefinition&) const = default;
};

struct PreparedGeneratorDefinitionSnapshot {
    std::string sourceId;
    uint32_t sourceRevision = 0;
    uint32_t definitionSchemaVersion = 0;
    std::string canonicalSnapshot;

    bool operator==(const PreparedGeneratorDefinitionSnapshot&) const = default;
};

GeneratorDefinition parseGeneratorDefinition(std::string_view yaml,
                                             std::string_view sourceName);

std::string serializeGeneratorDefinition(
    const GeneratorDefinition& definition);

PreparedGeneratorDefinitionSnapshot prepareGeneratorDefinitionSnapshot(
    const GeneratorDefinition& definition,
    const BlockRegistry& registry,
    GeneratorDefinitionOrigin origin);

GeneratorDefinitionData parseGeneratorDefinitionSnapshot(
    std::string_view snapshot,
    uint32_t definitionSchemaVersion,
    std::string_view sourceName);

void validateGeneratorDefinition(const GeneratorDefinition& definition,
                                 std::string_view sourceName);

void validateGeneratorDefinitionContent(const GeneratorDefinitionData& data,
                                        const BlockRegistry& registry,
                                        std::string_view sourceName);

} // namespace Rigel::Voxel
