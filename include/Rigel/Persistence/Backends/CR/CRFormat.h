#pragma once

#include "Rigel/Persistence/Format.h"

namespace Rigel::Persistence::Backends::CR {

const FormatDescriptor& descriptor();
FormatFactory factory();
FormatProbe probe();

} // namespace Rigel::Persistence::Backends::CR
