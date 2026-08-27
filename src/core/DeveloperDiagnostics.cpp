#include "DeveloperDiagnostics.h"

#include <cstdlib>
#include <string_view>

namespace Rigel::detail {

bool profilerEnabledFromEnvironmentValue(const char* value) noexcept {
    return value != nullptr && std::string_view(value) == "1";
}

bool profilerEnabledFromEnvironment() noexcept {
    return profilerEnabledFromEnvironmentValue(std::getenv("RIGEL_PROFILE"));
}

} // namespace Rigel::detail
