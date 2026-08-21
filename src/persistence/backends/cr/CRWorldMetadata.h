#pragma once

#include "Rigel/Persistence/Types.h"

#include <string>

namespace Rigel::Persistence::Backends::CR {

void requireSupportedDefaultZone(const PersistenceContext& context,
                                 const std::string& supportedZoneId);

} // namespace Rigel::Persistence::Backends::CR
