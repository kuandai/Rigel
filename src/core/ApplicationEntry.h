#pragma once

#include "Rigel/LaunchOptions.h"

namespace Rigel {

using ApplicationMain = void (*)();
using LaunchApplicationMain = void (*)(const LaunchOptions&);

int runApplication(ApplicationMain applicationMain) noexcept;
int runApplication(
    int argc,
    const char* const* argv,
    LaunchApplicationMain applicationMain) noexcept;
int runApplication(int argc, const char* const* argv) noexcept;
int runApplication() noexcept;

} // namespace Rigel
