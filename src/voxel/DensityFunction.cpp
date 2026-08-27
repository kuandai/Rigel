#include "Rigel/Voxel/DensityFunction.h"

#include "Rigel/Voxel/Noise.h"
#include "Rigel/Voxel/WorldGenerator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Rigel::Voxel {

namespace {
std::optional<DensityNodeType> parseNodeType(std::string_view type) {
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
    return std::nullopt;
}

std::optional<ClimateField> parseClimateField(std::string_view field) {
    if (field == "temperature") {
        return ClimateField::Temperature;
    }
    if (field == "humidity") {
        return ClimateField::Humidity;
    }
    if (field == "continentalness") {
        return ClimateField::Continentalness;
    }
    return std::nullopt;
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
        throw std::logic_error("Density evaluator has no graph");
    }
    auto it = m_graph->outputs.find(std::string(output));
    if (it == m_graph->outputs.end()) {
        throw std::logic_error(
            "Density evaluator output is unavailable: " + std::string(output));
    }
    return evaluateNode(it->second, ctx);
}

float DensityEvaluator::evaluateNode(int index, const DensitySampleContext& ctx) const {
    if (!m_graph || index < 0 || index >= static_cast<int>(m_graph->nodes.size())) {
        throw std::logic_error("Density evaluator node index is invalid");
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
            result = value;
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
            result = value;
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
            result = value;
            break;
        }
        case DensityNodeType::Add: {
            float sum = 0.0f;
            for (int input : node.inputs) {
                sum += evaluateNode(input, ctx);
            }
            result = sum;
            break;
        }
        case DensityNodeType::Mul: {
            float product = 1.0f;
            for (int input : node.inputs) {
                product *= evaluateNode(input, ctx);
            }
            result = product;
            break;
        }
        case DensityNodeType::Clamp: {
            result = std::clamp(
                evaluateNode(node.inputs.front(), ctx),
                node.minValue,
                node.maxValue);
            break;
        }
        case DensityNodeType::Max: {
            float current = -std::numeric_limits<float>::infinity();
            for (int input : node.inputs) {
                current = std::max(current, evaluateNode(input, ctx));
            }
            result = current;
            break;
        }
        case DensityNodeType::Min: {
            float current = std::numeric_limits<float>::infinity();
            for (int input : node.inputs) {
                current = std::min(current, evaluateNode(input, ctx));
            }
            result = current;
            break;
        }
        case DensityNodeType::Abs: {
            result = std::abs(evaluateNode(node.inputs.front(), ctx));
            break;
        }
        case DensityNodeType::Invert: {
            result = -evaluateNode(node.inputs.front(), ctx);
            break;
        }
        case DensityNodeType::Spline: {
            result = sampleSpline(
                node.splinePoints, evaluateNode(node.inputs.front(), ctx));
            break;
        }
        case DensityNodeType::Climate: {
            if (!ctx.climate) {
                throw std::logic_error(
                    "Density climate node requires a climate sample");
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

bool buildDensityGraph(const GeneratorDefinitionData& definition,
                       DensityGraph& graph,
                       std::string& error) {
    graph.nodes.clear();
    graph.nodeIndex.clear();
    graph.outputs.clear();
    error.clear();

    const auto& graphConfig = definition.densityGraph;
    graph.nodes.reserve(graphConfig.nodes.size());

    std::vector<std::vector<std::string>> pendingInputs;
    pendingInputs.reserve(graphConfig.nodes.size());

    for (const auto& nodeConfig : graphConfig.nodes) {
        DensityNode node;
        node.name = nodeConfig.id;
        const auto type = parseNodeType(nodeConfig.type);
        if (!type) {
            error = "Unknown density node type: " + nodeConfig.type;
            return false;
        }
        node.type = *type;
        node.noise = nodeConfig.noise;
        node.value = nodeConfig.value;
        node.minValue = nodeConfig.minValue;
        node.maxValue = nodeConfig.maxValue;
        node.scale = nodeConfig.scale;
        node.offset = nodeConfig.offset;
        node.splinePoints = nodeConfig.splinePoints;
        if (node.type == DensityNodeType::Spline) {
            if (node.splinePoints.empty()) {
                error = "Density spline node '" + node.name +
                    "' requires at least one point";
                return false;
            }
            for (const auto& [x, y] : node.splinePoints) {
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    error = "Density spline node '" + node.name +
                        "' requires finite point coordinates";
                    return false;
                }
            }
            std::stable_sort(
                node.splinePoints.begin(), node.splinePoints.end(),
                [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
            for (size_t point = 1; point < node.splinePoints.size(); ++point) {
                if (!(node.splinePoints[point - 1].first <
                      node.splinePoints[point].first)) {
                    error = "Density spline node '" + node.name +
                        "' requires unique X coordinates";
                    return false;
                }
            }
        }
        if (node.type == DensityNodeType::Climate) {
            const auto field = parseClimateField(nodeConfig.field);
            if (!field) {
                error = "Unknown density climate field: " + nodeConfig.field;
                return false;
            }
            node.climateField = *field;
        }
        if (!graph.nodeIndex.emplace(
                 nodeConfig.id, static_cast<int>(graph.nodes.size())).second) {
            error = "Duplicate density node ID: " + nodeConfig.id;
            return false;
        }
        graph.nodes.push_back(std::move(node));
        pendingInputs.push_back(nodeConfig.inputs);
    }

    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        for (const auto& input : pendingInputs[i]) {
            auto it = graph.nodeIndex.find(input);
            if (it == graph.nodeIndex.end()) {
                error = "Missing density node input: " + input;
                return false;
            } else {
                graph.nodes[i].inputs.push_back(it->second);
            }
        }
    }

    if (error.empty()) {
        enum class VisitState : uint8_t { Unvisited, Visiting, Complete };
        std::vector<VisitState> states(graph.nodes.size(), VisitState::Unvisited);
        std::function<bool(int)> visit = [&](int index) {
            auto& state = states[static_cast<size_t>(index)];
            if (state == VisitState::Visiting) {
                error = "Density graph cycle detected at node: " +
                    graph.nodes[static_cast<size_t>(index)].name;
                return false;
            }
            if (state == VisitState::Complete) {
                return true;
            }
            state = VisitState::Visiting;
            for (int input : graph.nodes[static_cast<size_t>(index)].inputs) {
                if (!visit(input)) {
                    return false;
                }
            }
            state = VisitState::Complete;
            return true;
        };
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (!visit(static_cast<int>(i))) {
                return false;
            }
        }
    }

    for (const auto& output : graphConfig.outputs) {
        auto it = graph.nodeIndex.find(output.node);
        if (it != graph.nodeIndex.end()) {
            if (!graph.outputs.emplace(output.semantic, it->second).second) {
                error = "Duplicate density output semantic: " + output.semantic;
                return false;
            }
        } else {
            error = "Missing density output node: " + output.node;
            return false;
        }
    }

    return error.empty();
}

} // namespace Rigel::Voxel
