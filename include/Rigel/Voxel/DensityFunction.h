#pragma once

#include "WorldGenConfig.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Voxel {

struct ClimateSample;

enum class DensityNodeType {
    Constant,
    Noise2D,
    Noise3D,
    Noise3DXY,
    Add,
    Mul,
    Clamp,
    Max,
    Min,
    Abs,
    Invert,
    Spline,
    Climate,
    Y
};

enum class ClimateField {
    Temperature,
    Humidity,
    Continentalness
};

struct DensityNode {
    DensityNodeType type = DensityNodeType::Constant;
    std::vector<int> inputs;
    WorldGenConfig::NoiseConfig noise;
    ClimateField climateField = ClimateField::Temperature;
    float value = 0.0f;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    float scale = 1.0f;
    float offset = 0.0f;
    std::vector<std::pair<float, float>> splinePoints;
    std::string name;
};

struct DensityGraph {
    std::vector<DensityNode> nodes;
    std::unordered_map<std::string, int> nodeIndex;
    std::unordered_map<std::string, int> outputs;

    bool empty() const { return nodes.empty(); }
};

struct DensitySampleContext {
    int worldX = 0;
    int worldY = 0;
    int worldZ = 0;
    const ClimateSample* climate = nullptr;
    struct NoiseSampleCache {
        virtual ~NoiseSampleCache() = default;
        virtual bool sampleNoise3D(int nodeIndex, int worldX, int worldY, int worldZ,
                                   float& outValue) const = 0;
    };
    const NoiseSampleCache* noiseCache = nullptr;
};

class DensityEvaluator {
public:
    DensityEvaluator() = default;
    DensityEvaluator(const DensityGraph* graph, uint32_t seed);

    void reset(const DensityGraph* graph, uint32_t seed);
    void beginSample() const;

    float evaluateOutput(std::string_view output, const DensitySampleContext& ctx) const;
    float evaluateNode(int index, const DensitySampleContext& ctx) const;

private:
    const DensityGraph* m_graph = nullptr;
    uint32_t m_seed = 0;
    mutable std::vector<float> m_cache;
    mutable std::vector<int> m_stamp;
    mutable int m_stampValue = 1;
};

bool buildDensityGraph(const WorldGenConfig& config,
                       DensityGraph& graph,
                       std::string& error);

} // namespace Rigel::Voxel
