#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace Rigel::Input {

inline constexpr std::size_t kMaximumBindingTokenBytes = 64;

enum class PhysicalInputType {
    Keyboard,
    MouseButton,
};

struct PhysicalInput {
    PhysicalInputType type = PhysicalInputType::Keyboard;
    int code = 0;

    bool operator==(const PhysicalInput&) const = default;
};

std::optional<PhysicalInput> decodeBindingToken(std::string_view token);

} // namespace Rigel::Input
