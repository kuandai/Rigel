#pragma once

#include "Rigel/Persistence/Providers.h"

namespace Rigel::Persistence::Backends::CR {

inline constexpr const char* kCRSettingsProviderId = "rigel:persistence.cr";

struct CRPersistenceSettings final : public Provider {
    bool enableLz4 = false;
};

} // namespace Rigel::Persistence::Backends::CR
