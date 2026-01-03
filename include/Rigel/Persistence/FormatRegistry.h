#pragma once

#include "Rigel/Persistence/Format.h"

#include <unordered_map>

namespace Rigel::Persistence {

class FormatRegistry {
public:
    void registerFormat(const FormatDescriptor& descriptor, FormatFactory factory, FormatProbe probe);

    std::unique_ptr<PersistenceFormat> resolveFormat(const PersistenceContext& context) const;

private:
    struct Entry {
        FormatDescriptor descriptor;
        FormatFactory factory;
        FormatProbe probe;
    };

    std::optional<ProbeResult> probeFromManifest(const Entry& entry, const PersistenceContext& context) const;
    std::optional<ProbeResult> probeFromStorage(const Entry& entry, StorageBackend& storage, const PersistenceContext& context) const;

    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace Rigel::Persistence
