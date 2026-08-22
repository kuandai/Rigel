#pragma once

#include <string>
#include <string_view>

namespace Rigel::Persistence::detail {

void validateZoneIdentifier(std::string_view zoneId);
std::string zoneIdentifierStoragePath(std::string_view zoneId);

} // namespace Rigel::Persistence::detail
