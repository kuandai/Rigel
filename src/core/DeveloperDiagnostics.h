#pragma once

namespace Rigel::detail {

// Profiling is developer tooling rather than render, world, or player
// configuration. Only the explicit value "1" enables collection.
bool profilerEnabledFromEnvironment() noexcept;
bool profilerEnabledFromEnvironmentValue(const char* value) noexcept;

} // namespace Rigel::detail
