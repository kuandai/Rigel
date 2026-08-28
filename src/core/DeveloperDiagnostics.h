#pragma once

#include <cstdint>
#include <filesystem>

namespace Rigel::detail {

// Profiling is developer tooling rather than render, world, or player
// configuration. Only the explicit value "1" enables collection.
bool profilerEnabledFromEnvironment() noexcept;
bool profilerEnabledFromEnvironmentValue(const char* value) noexcept;

// Reports only the finite set of obsolete configuration paths that Rigel
// previously loaded from the launch directory. This deliberately does not
// enumerate directories or parse legacy configuration documents.
void warnAboutObsoleteConfiguration(
    const std::filesystem::path& launchDirectory,
    std::uint32_t activeWorldId);

} // namespace Rigel::detail
