#pragma once

#include <cstddef>
#include <cstdint>

namespace Rigel::Persistence::detail {

inline constexpr size_t MaxEntityJournalEncodedBytes = 64 * 1024 * 1024;
inline constexpr size_t MaxEntityJournalPayloadBytes =
    MaxEntityJournalEncodedBytes;
inline constexpr uint32_t MaxEntityJournalRegions = 4'096;
inline constexpr size_t MaxEntityJournalChunks = 65'536;
inline constexpr size_t MaxEntityJournalEntities = 65'536;

} // namespace Rigel::Persistence::detail
