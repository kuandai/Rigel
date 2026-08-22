#include "Rigel/Persistence/FormatRegistry.h"
#include "Rigel/Persistence/Storage.h"

#include <stdexcept>

namespace Rigel::Persistence {

void FormatRegistry::registerFormat(const FormatDescriptor& descriptor, FormatFactory factory, FormatProbe probe) {
    if (!factory) {
        throw std::runtime_error("FormatRegistry: factory is required");
    }
    Entry entry{descriptor, std::move(factory), std::move(probe)};
    m_entries[descriptor.id] = std::move(entry);
}

std::unique_ptr<PersistenceFormat> FormatRegistry::resolveFormat(const PersistenceContext& context) const {
    if (!context.storage) {
        throw std::runtime_error("FormatRegistry: no storage backend provided");
    }

    if (!context.preferredFormat.empty()) {
        auto it = m_entries.find(context.preferredFormat);
        if (it == m_entries.end()) {
            throw std::runtime_error("FormatRegistry: preferred format not registered: " + context.preferredFormat);
        }
        return it->second.factory(context);
    }

    std::optional<ProbeResult> bestProbe;
    const Entry* bestEntry = nullptr;
    for (const auto& [id, entry] : m_entries) {
        auto probeResult = probeFromStorage(entry, *context.storage, context);
        if (!probeResult) {
            continue;
        }
        if (!bestProbe || probeResult->confidence > bestProbe->confidence) {
            bestProbe = probeResult;
            bestEntry = &entry;
        }
    }

    if (!bestEntry) {
        throw std::runtime_error("FormatRegistry: unable to detect persistence format");
    }

    return bestEntry->factory(context);
}

std::optional<ProbeResult> FormatRegistry::probeFromStorage(const Entry& entry, StorageBackend& storage, const PersistenceContext& context) const {
    if (!entry.probe) {
        return std::nullopt;
    }
    return entry.probe(storage, context);
}

} // namespace Rigel::Persistence
