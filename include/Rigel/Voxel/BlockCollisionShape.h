#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
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

    static constexpr float MinimumCoordinate = -0.25f;
    static constexpr float MaximumCoordinate = 1.25f;

    /** The default preserves the legacy solid-block contract. */
    BlockCollisionShape() noexcept = default;
    BlockCollisionShape(const BlockCollisionShape&) noexcept = default;
    BlockCollisionShape& operator=(const BlockCollisionShape&) noexcept = default;

    BlockCollisionShape(BlockCollisionShape&& other) noexcept
        : m_kind(other.m_kind), m_boxes(other.m_boxes) {}

    BlockCollisionShape& operator=(BlockCollisionShape&& other) noexcept {
        m_kind = other.m_kind;
        m_boxes = other.m_boxes;
        return *this;
    }

    static BlockCollisionShape empty() noexcept {
        return BlockCollisionShape(Kind::Empty);
    }

    static BlockCollisionShape fullCube() noexcept {
        return BlockCollisionShape(Kind::FullCube);
    }

    static BlockCollisionShape boxes(
        const std::vector<BlockCollisionBox>& boxes
    ) {
        if (boxes.empty()) {
            throw std::invalid_argument(
                "collision boxes must contain at least one box");
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
            return fullCube();
        }

        BlockCollisionShape result(Kind::Boxes);
        result.m_boxes =
            std::make_shared<const std::vector<BlockCollisionBox>>(
                boxes);
        return result;
    }

    Kind kind() const noexcept { return m_kind; }
    bool isEmpty() const noexcept { return m_kind == Kind::Empty; }
    bool isFullCube() const noexcept { return m_kind == Kind::FullCube; }
    bool isBoxes() const noexcept { return m_kind == Kind::Boxes; }

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
    explicit BlockCollisionShape(Kind kind) noexcept : m_kind(kind) {}

    static const BlockCollisionBox& fullCubeBox() noexcept {
        static constexpr BlockCollisionBox box{
            .min = {0.0f, 0.0f, 0.0f},
            .max = {1.0f, 1.0f, 1.0f},
        };
        return box;
    }

    Kind m_kind = Kind::FullCube;
    std::shared_ptr<const std::vector<BlockCollisionBox>> m_boxes;
};

} // namespace Rigel::Voxel
