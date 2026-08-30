#pragma once

#include "Block.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rigel::Voxel {

struct BlockModelBounds {
    std::array<float, 3> min{};
    std::array<float, 3> max{};

    bool operator==(const BlockModelBounds&) const = default;
};

struct BlockModelUvRect {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;

    bool operator==(const BlockModelUvRect&) const = default;
};

enum class BlockModelUvRotation : uint8_t {
    // Quarter turns in the normalized block-model face convention, applied
    // after selecting the UV rectangle.
    None = 0,
    Quarter = 1,
    Half = 2,
    ThreeQuarter = 3,
};

struct BlockModelFace {
    std::string textureSlot;
    BlockModelUvRect uv;
    BlockModelUvRotation rotation = BlockModelUvRotation::None;
    std::optional<Direction> shadingFace;
    bool ambientOcclusion = false;
    bool cullAgainstOpaqueNeighbor = false;
};

struct BlockModelCuboid {
    BlockModelBounds bounds;
    std::array<std::optional<BlockModelFace>, DirectionCount> faces;
};

/**
 * Immutable, normalized visual geometry shared by block registrations.
 * Coordinates are expressed in block-cell units and intentionally may extend
 * outside the unit cell.
 */
class BlockModel final {
public:
    BlockModel(std::string identifier,
               std::vector<std::string> textureSlots,
               std::vector<BlockModelCuboid> cuboids)
        : m_identifier(std::move(identifier))
        , m_textureSlots(std::move(textureSlots))
        , m_cuboids(std::move(cuboids)) {}

    const std::string& identifier() const { return m_identifier; }
    const std::vector<std::string>& textureSlots() const { return m_textureSlots; }
    const std::vector<BlockModelCuboid>& cuboids() const { return m_cuboids; }
    bool isEmpty() const { return m_cuboids.empty(); }
    bool isFullCube() const { return m_builtinFullCube; }

    static std::shared_ptr<const BlockModel> fullCube() {
        static const std::shared_ptr<const BlockModel> model = [] {
            BlockModelCuboid cuboid;
            cuboid.bounds.max = {1.0f, 1.0f, 1.0f};
            std::vector<std::string> slots;
            slots.reserve(DirectionCount);
            for (size_t index = 0; index < DirectionCount; ++index) {
                const Direction direction = static_cast<Direction>(index);
                std::string slot(directionName(direction));
                slots.push_back(slot);
                cuboid.faces[index] = BlockModelFace{
                    .textureSlot = std::move(slot),
                    .ambientOcclusion = true,
                    .cullAgainstOpaqueNeighbor = true,
                };
            }
            auto* value = new BlockModel(
                "builtin:full_cube", std::move(slots), {std::move(cuboid)});
            value->m_builtinFullCube = true;
            return std::shared_ptr<const BlockModel>(value);
        }();
        return model;
    }

    static std::shared_ptr<const BlockModel> empty() {
        static const std::shared_ptr<const BlockModel> model =
            std::make_shared<const BlockModel>(
                "builtin:empty", std::vector<std::string>{},
                std::vector<BlockModelCuboid>{});
        return model;
    }

    static constexpr std::string_view directionName(Direction direction) {
        constexpr std::array<std::string_view, DirectionCount> names = {
            "pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z"
        };
        return names[static_cast<size_t>(direction)];
    }

    static std::optional<Direction> directionFromName(std::string_view name) {
        for (size_t index = 0; index < DirectionCount; ++index) {
            if (name == directionName(static_cast<Direction>(index))) {
                return static_cast<Direction>(index);
            }
        }
        return std::nullopt;
    }

private:
    std::string m_identifier;
    std::vector<std::string> m_textureSlots;
    std::vector<BlockModelCuboid> m_cuboids;
    bool m_builtinFullCube = false;
};

/** Right-angle orientations present in normalized block registrations. */
enum class BlockModelOrientation : uint8_t {
    Identity,
    RotateX90,
    RotateX270,
    RotateY90,
    RotateY180,
    RotateY270,
    RotateZ90,
};

/**
 * A registration's immutable reference to reusable visual geometry.
 *
 * Orientation is deliberately a closed set rather than a general transform.
 * rotateTopBottomUv preserves the source-authored top/bottom UV correction
 * independently from the geometric orientation.
 */
struct BlockModelInstance {
    std::shared_ptr<const BlockModel> geometry = BlockModel::fullCube();
    BlockModelOrientation orientation = BlockModelOrientation::Identity;
    bool rotateTopBottomUv = false;

    BlockModelInstance() = default;
    BlockModelInstance(std::shared_ptr<const BlockModel> value)
        : geometry(std::move(value)) {}

    BlockModelInstance& operator=(std::shared_ptr<const BlockModel> value) {
        geometry = std::move(value);
        return *this;
    }

    explicit operator bool() const { return static_cast<bool>(geometry); }
    const BlockModel* operator->() const { return geometry.get(); }
    const BlockModel* get() const { return geometry.get(); }
};

class BlockModelRegistrationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Models are registered during resource initialization, then frozen. */
class BlockModelRegistry final {
public:
    BlockModelRegistry() {
        m_models.emplace(BlockModel::fullCube()->identifier(), BlockModel::fullCube());
        m_models.emplace(BlockModel::empty()->identifier(), BlockModel::empty());
    }

    std::shared_ptr<const BlockModel> find(std::string_view identifier) const {
        if (identifier == "cube") {
            return BlockModel::fullCube();
        }
        if (identifier == "none") {
            return BlockModel::empty();
        }
        const auto found = m_models.find(std::string(identifier));
        return found == m_models.end() ? nullptr : found->second;
    }

    bool contains(std::string_view identifier) const {
        return static_cast<bool>(find(identifier));
    }

    void registerModels(
        std::span<const std::shared_ptr<const BlockModel>> models
    ) {
        if (m_frozen) {
            throw BlockModelRegistrationError("Block model registry is frozen");
        }
        std::unordered_set<std::string> candidateIds;
        for (const auto& model : models) {
            if (!model || model->identifier().empty()) {
                throw BlockModelRegistrationError(
                    "Block model registration requires a model with an identifier");
            }
            if (contains(model->identifier()) ||
                !candidateIds.insert(model->identifier()).second) {
                throw BlockModelRegistrationError(
                    "Block model identifier already registered: " +
                    model->identifier());
            }
        }

        m_models.reserve(m_models.size() + models.size());
        try {
            for (const auto& model : models) {
                m_models.emplace(model->identifier(), model);
            }
        } catch (...) {
            for (const auto& model : models) {
                if (model) m_models.erase(model->identifier());
            }
            throw;
        }
    }

    size_t size() const { return m_models.size(); }
    bool frozen() const { return m_frozen; }
    void freeze() { m_frozen = true; }

    void swap(BlockModelRegistry& other) noexcept {
        m_models.swap(other.m_models);
        std::swap(m_frozen, other.m_frozen);
    }

private:
    std::unordered_map<std::string, std::shared_ptr<const BlockModel>> m_models;
    bool m_frozen = false;
};

} // namespace Rigel::Voxel
