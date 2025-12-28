#include "Rigel/Voxel/DensityFunction.h"

#include "Rigel/Voxel/Noise.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Rigel::Voxel {

namespace {
DensityNodeType parseNodeType(std::string_view type) {
    if (type == "constant") {
        return DensityNodeType::Constant;
    }
    if (type == "noise2d") {
        return DensityNodeType::Noise2D;
    }
    if (type == "noise3d") {
        return DensityNodeType::Noise3D;
    }
    if (type == "noise3d_xy") {
        return DensityNodeType::Noise3DXY;
    }
    if (type == "add") {
        return DensityNodeType::Add;
    }
    if (type == "mul") {
        return DensityNodeType::Mul;
    }
    if (type == "clamp") {
        return DensityNodeType::Clamp;
    }
    if (type == "max") {
        return DensityNodeType::Max;
    }
    if (type == "min") {
        return DensityNodeType::Min;
    }
    if (type == "abs") {
        return DensityNodeType::Abs;
    }
    if (type == "invert") {
        return DensityNodeType::Invert;
    }
    if (type == "spline") {
        return DensityNodeType::Spline;
    }
    if (type == "climate") {
        return DensityNodeType::Climate;
    }
    if (type == "y") {
        return DensityNodeType::Y;
    }
    return DensityNodeType::Constant;
}

ClimateField parseClimateField(std::string_view field) {
    if (field == "temperature") {
        return ClimateField::Temperature;
    }
    if (field == "humidity") {
        return ClimateField::Humidity;
    }
    if (field == "continentalness") {
        return ClimateField::Continentalness;
    }
    return ClimateField::Temperature;
}

float sampleSpline(const std::vector<std::pair<float, float>>& points, float x) {
    if (points.empty()) {
        return x;
    }
    if (points.size() == 1) {
        return points.front().second;
    }
    if (x <= points.front().first) {
        return points.front().second;
    }
    if (x >= points.back().first) {
        return points.back().second;
    }
    for (size_t i = 1; i < points.size(); ++i) {
        const auto& a = points[i - 1];
        const auto& b = points[i];
        if (x <= b.first) {
            float t = (x - a.first) / (b.first - a.first);
            return a.second + (b.second - a.second) * t;
        }
    }
    return points.back().second;
}
} // namespace

DensityEvaluator::DensityEvaluator(const DensityGraph* graph, uint32_t seed) {
    reset(graph, seed);
}

void DensityEvaluator::reset(const DensityGraph* graph, uint32_t seed) {
    m_graph = graph;
    m_seed = seed;
    m_cache.clear();
    m_stamp.clear();
    m_stampValue = 1;
    if (m_graph) {
        m_cache.resize(m_graph->nodes.size(), 0.0f);
        m_stamp.resize(m_graph->nodes.size(), 0);
    }
}

void DensityEvaluator::beginSample() const {
    if (!m_graph) {
        return;
    }
    if (m_stampValue == std::numeric_limits<int>::max()) {
        std::fill(m_stamp.begin(), m_stamp.end(), 0);
        m_stampValue = 1;
    } else {
        ++m_stampValue;
    }
}

float DensityEvaluator::evaluateOutput(std::string_view output, const DensitySampleContext& ctx) const {
    if (!m_graph) {
        return 0.0f;
    }
    auto it = m_graph->outputs.find(std::string(output));
    if (it == m_graph->outputs.end()) {
        return 0.0f;
    }
    return evaluateNode(it->second, ctx);
}

float DensityEvaluator::evaluateNode(int index, const DensitySampleContext& ctx) const {
    if (!m_graph || index < 0 || index >= static_cast<int>(m_graph->nodes.size())) {
        return 0.0f;
    }
    if (m_stamp[index] == m_stampValue) {
        return m_cache[static_cast<size_t>(index)];
    }
    m_stamp[index] = m_stampValue;

    const DensityNode& node = m_graph->nodes[static_cast<size_t>(index)];
    float result = 0.0f;

    switch (node.type) {
        case DensityNodeType::Constant:
            result = node.value;
            break;
        case DensityNodeType::Noise2D: {
            uint32_t seed = Noise::seedForChannel(m_seed, node.name);
            float value = Noise::fbm2D(
                static_cast<float>(ctx.worldX),
                static_cast<float>(ctx.worldZ),
                seed,
                node.noise
            );
            result = value * node.scale + node.offset;
            break;
        }
        case DensityNodeType::Noise3D: {
            uint32_t seed = Noise::seedForChannel(m_seed, node.name);
            float value = 0.0f;
            bool usedCache = false;
            if (ctx.noiseCache) {
                usedCache = ctx.noiseCache->sampleNoise3D(
                    index, ctx.worldX, ctx.worldY, ctx.worldZ, value);
            }
            if (!usedCache) {
                value = Noise::fbm3D(
                    static_cast<float>(ctx.worldX),
                    static_cast<float>(ctx.worldY),
                    static_cast<float>(ctx.worldZ),
                    seed,
                    node.noise
                );
            }
            result = value * node.scale + node.offset;
            break;
        }
        case DensityNodeType::Noise3DXY: {
            uint32_t seed = Noise::seedForChannel(m_seed, node.name);
            float value = Noise::fbm3D(
                static_cast<float>(ctx.worldX),
                static_cast<float>(ctx.worldY),
                0.0f,
                seed,
                node.noise
            );
            result = value * node.scale + node.offset;
            break;
        }
        case DensityNodeType::Add: {
            float sum = 0.0f;
            for (int input : node.inputs) {
                if (input >= 0) {
                    sum += evaluateNode(input, ctx);
                }
            }
            result = sum;
            break;
        }
        case DensityNodeType::Mul: {
            float product = 1.0f;
            bool hasInput = false;
            for (int input : node.inputs) {
                if (input >= 0) {
                    product *= evaluateNode(input, ctx);
                    hasInput = true;
                }
            }
            result = hasInput ? product : 0.0f;
            break;
        }
        case DensityNodeType::Clamp: {
            float value = node.inputs.empty() ? 0.0f : evaluateNode(node.inputs.front(), ctx);
            float minValue = node.minValue;
            float maxValue = node.maxValue;
            if (minValue > maxValue) {
                std::swap(minValue, maxValue);
            }
            result = std::clamp(value, minValue, maxValue);
            break;
        }
        case DensityNodeType::Max: {
            float current = -std::numeric_limits<float>::infinity();
            for (int input : node.inputs) {
                if (input >= 0) {
                    current = std::max(current, evaluateNode(input, ctx));
                }
            }
            result = (current == -std::numeric_limits<float>::infinity()) ? 0.0f : current;
            break;
        }
        case DensityNodeType::Min: {
            float current = std::numeric_limits<float>::infinity();
            for (int input : node.inputs) {
                if (input >= 0) {
                    current = std::min(current, evaluateNode(input, ctx));
                }
            }
            result = (current == std::numeric_limits<float>::infinity()) ? 0.0f : current;
            break;
        }
        case DensityNodeType::Abs: {
            float value = node.inputs.empty() ? 0.0f : evaluateNode(node.inputs.front(), ctx);
            result = std::abs(value);
            break;
        }
        case DensityNodeType::Invert: {
            float value = node.inputs.empty() ? 0.0f : evaluateNode(node.inputs.front(), ctx);
            result = -value;
            break;
        }
        case DensityNodeType::Spline: {
            float value = node.inputs.empty() ? 0.0f : evaluateNode(node.inputs.front(), ctx);
            result = sampleSpline(node.splinePoints, value);
            break;
        }
        case DensityNodeType::Climate: {
            if (!ctx.climate) {
                result = 0.0f;
                break;
            }
            switch (node.climateField) {
                case ClimateField::Temperature:
                    result = ctx.climate->temperature;
                    break;
                case ClimateField::Humidity:
                    result = ctx.climate->humidity;
                    break;
                case ClimateField::Continentalness:
                    result = ctx.climate->continentalness;
                    break;
            }
            break;
        }
        case DensityNodeType::Y:
            result = static_cast<float>(ctx.worldY) * node.scale + node.offset;
            break;
    }

    m_cache[static_cast<size_t>(index)] = result;
    return result;
}

bool buildDensityGraph(const WorldGenConfig& config, DensityGraph& graph, std::string& error) {
    graph.nodes.clear();
    graph.nodeIndex.clear();
    graph.outputs.clear();
    error.clear();

    const auto& graphConfig = config.densityGraph;
    graph.nodes.reserve(graphConfig.nodes.size());

    std::vector<std::vector<std::string>> pendingInputs;
    pendingInputs.reserve(graphConfig.nodes.size());

    for (const auto& nodeConfig : graphConfig.nodes) {
        DensityNode node;
        node.name = nodeConfig.id;
        node.type = parseNodeType(nodeConfig.type);
        node.noise = nodeConfig.noise;
        node.value = nodeConfig.value;
        node.minValue = nodeConfig.minValue;
        node.maxValue = nodeConfig.maxValue;
        node.scale = nodeConfig.scale;
        node.offset = nodeConfig.offset;
        node.splinePoints = nodeConfig.splinePoints;
        if (!node.splinePoints.empty()) {
            std::sort(node.splinePoints.begin(), node.splinePoints.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        if (node.type == DensityNodeType::Climate) {
            node.climateField = parseClimateField(nodeConfig.field);
        }
        graph.nodeIndex[nodeConfig.id] = static_cast<int>(graph.nodes.size());
        graph.nodes.push_back(std::move(node));
        pendingInputs.push_back(nodeConfig.inputs);
    }

    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        for (const auto& input : pendingInputs[i]) {
            auto it = graph.nodeIndex.find(input);
            if (it == graph.nodeIndex.end()) {
                graph.nodes[i].inputs.push_back(-1);
                if (error.empty()) {
                    error = "Missing density node input: " + input;
                }
            } else {
                graph.nodes[i].inputs.push_back(it->second);
            }
        }
    }

    for (const auto& output : graphConfig.outputs) {
        auto it = graph.nodeIndex.find(output.second);
        if (it != graph.nodeIndex.end()) {
            graph.outputs[output.first] = it->second;
        } else if (error.empty()) {
            error = "Missing density output node: " + output.second;
        }
    }

    return error.empty();
}

} // namespace Rigel::Voxel
