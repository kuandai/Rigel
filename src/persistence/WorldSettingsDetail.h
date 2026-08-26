#pragma once

#include "Rigel/Persistence/Types.h"

namespace Rigel::Persistence::detail {

// Test seam for exercising cleanup behavior independently of format-aware
// publication recovery. Production startup uses the public, format-aware
// recovery/bootstrap lifecycle.
void recoverAbandonedWorldGenerationStagingForTesting(
    const PersistenceContext& context);

} // namespace Rigel::Persistence::detail
