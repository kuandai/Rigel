#pragma once

namespace Rigel {

using ApplicationMain = void (*)();

int runApplication(ApplicationMain applicationMain) noexcept;
int runApplication() noexcept;

} // namespace Rigel
