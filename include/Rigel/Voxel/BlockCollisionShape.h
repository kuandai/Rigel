#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Rigel::Voxel {

/**
 * The single tolerance for static block contact.
 *
 * Candidate-range expansion, contact separation, orthogonal overlap tests,
 * and support probes all derive from this value.
 */
inline constexpr float BlockCollisionContactTolerance = 1.0e-4f;

/** An axis-aligned collision box in block-cell coordinates. */
struct BlockCollisionBox {
    std::array<float, 3> min{};
    std::array<float, 3> max{};

    bool operator==(const BlockCollisionBox&) const = default;
};

/**
 * Immutable physical geometry for a block registration.
 *
 * Empty and FullCube do not own box storage. Custom boxes are validated once,
 * retained through immutable shared storage, and exposed as a read-only span.
 */
class BlockCollisionShape final {
public:
    enum class Kind : unsigned char {
        Empty,
        FullCube,
        Boxes,
    };

    /** How the normalized physical geometry was obtained. */
    enum class Provenance : unsigned char {
        Authored,
        Exact,
        ConservativeFallback,
    };

    static constexpr float MinimumCoordinate = -0.25f;
    static constexpr float MaximumCoordinate = 1.25f;
    static constexpr size_t MaximumBoxes = 16;

    /** A default-constructed block shape is the canonical full cube. */
    BlockCollisionShape() noexcept = default;
    BlockCollisionShape(const BlockCollisionShape&) noexcept = default;
    BlockCollisionShape& operator=(const BlockCollisionShape&) noexcept = default;

    BlockCollisionShape(BlockCollisionShape&&) noexcept = default;
    BlockCollisionShape& operator=(BlockCollisionShape&&) noexcept = default;

    static BlockCollisionShape empty(
        Provenance provenance = Provenance::Authored
    ) noexcept {
        return BlockCollisionShape(Kind::Empty, provenance);
    }

    static BlockCollisionShape fullCube(
        Provenance provenance = Provenance::Authored
    ) noexcept {
        return BlockCollisionShape(Kind::FullCube, provenance);
    }

    static BlockCollisionShape boxes(
        const std::vector<BlockCollisionBox>& boxes,
        Provenance provenance = Provenance::Authored
    ) {
        if (boxes.empty()) {
            throw std::invalid_argument(
                "collision boxes must contain at least one box");
        }
        if (boxes.size() > MaximumBoxes) {
            throw std::invalid_argument(
                "collision shapes support at most " +
                std::to_string(MaximumBoxes) + " boxes; received " +
                std::to_string(boxes.size()));
        }

        for (size_t index = 0; index < boxes.size(); ++index) {
            const BlockCollisionBox& box = boxes[index];
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(box.min[axis]) ||
                    !std::isfinite(box.max[axis])) {
                    throw std::invalid_argument(
                        "collision box coordinates must be finite");
                }
                if (box.min[axis] < MinimumCoordinate ||
                    box.max[axis] > MaximumCoordinate) {
                    throw std::invalid_argument(
                        "collision box coordinates exceed the supported "
                        "[-0.25, 1.25] range");
                }
                if (box.min[axis] >= box.max[axis]) {
                    throw std::invalid_argument(
                        "collision boxes must have positive volume");
                }
            }
            for (size_t previous = 0; previous < index; ++previous) {
                if (boxes[previous] == box) {
                    throw std::invalid_argument(
                        "collision boxes must not contain duplicates");
                }
            }
        }

        if (boxes.size() == 1 && boxes.front() == fullCubeBox()) {
            return fullCube(provenance);
        }

        BlockCollisionShape result(Kind::Boxes, provenance);
        result.m_boxes =
            std::make_shared<const std::vector<BlockCollisionBox>>(
                boxes);
        return result;
    }

    Kind kind() const noexcept {
        if (m_boxes) {
            return Kind::Boxes;
        }
        return m_isFullCube ? Kind::FullCube : Kind::Empty;
    }
    Provenance provenance() const noexcept { return m_provenance; }
    bool isEmpty() const noexcept { return !m_isFullCube && !m_boxes; }
    bool isFullCube() const noexcept { return m_isFullCube; }
    bool isBoxes() const noexcept { return static_cast<bool>(m_boxes); }

    /** Iteration is allocation-free for every shape kind. */
    std::span<const BlockCollisionBox> boxes() const noexcept {
        if (isEmpty()) {
            return {};
        }
        if (isFullCube()) {
            return {&fullCubeBox(), 1};
        }
        return *m_boxes;
    }

private:
    explicit BlockCollisionShape(
        Kind kind,
        Provenance provenance
    ) noexcept
        : m_isFullCube(kind == Kind::FullCube)
        , m_provenance(provenance) {}

    static const BlockCollisionBox& fullCubeBox() noexcept {
        static constexpr BlockCollisionBox box{
            .min = {0.0f, 0.0f, 0.0f},
            .max = {1.0f, 1.0f, 1.0f},
        };
        return box;
    }

    // With no custom box owner, this flag distinguishes FullCube from Empty.
    // A default-moved custom shape therefore becomes a coherent Empty shape.
    bool m_isFullCube = true;
    Provenance m_provenance = Provenance::Authored;
    std::shared_ptr<const std::vector<BlockCollisionBox>> m_boxes;
};

} // namespace Rigel::Voxel
