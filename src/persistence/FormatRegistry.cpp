#include "Rigel/Persistence/FormatRegistry.h"
#include "Rigel/Persistence/Storage.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace Rigel::Persistence {

namespace {

struct ManifestInfo {
    std::string formatId;
    int version = 0;
};

std::optional<std::string> extractStringField(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find('"', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto end = text.find('"', pos + 1);
    if (end == std::string::npos || end <= pos + 1) {
        return std::nullopt;
    }
    return text.substr(pos + 1, end - pos - 1);
}

std::optional<int> extractIntField(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = text.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    while (pos < text.size() && (text[pos] == ':' || std::isspace(static_cast<unsigned char>(text[pos])))) {
        ++pos;
    }
    if (pos >= text.size() || (!std::isdigit(static_cast<unsigned char>(text[pos])) && text[pos] != '-')) {
        return std::nullopt;
    }
    size_t end = pos;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
        ++end;
    }
    try {
        return std::stoi(text.substr(pos, end - pos));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<ManifestInfo> readManifest(StorageBackend& storage, const PersistenceContext& context) {
    std::string manifestPath = context.manifestPath.empty()
        ? (context.rootPath + "/format.json")
        : context.manifestPath;

    if (!storage.exists(manifestPath)) {
        return std::nullopt;
    }

    auto reader = storage.openRead(manifestPath);
    if (!reader) {
        return std::nullopt;
    }

    std::vector<uint8_t> bytes(reader->size());
    reader->seek(0);
    if (!bytes.empty()) {
        reader->readBytes(bytes.data(), bytes.size());
    }

    std::string text(bytes.begin(), bytes.end());
    auto formatId = extractStringField(text, "formatId");
    auto version = extractIntField(text, "version");
    if (!formatId || !version) {
        return std::nullopt;
    }

    return ManifestInfo{*formatId, *version};
}

} // namespace

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

    if (auto manifest = readManifest(*context.storage, context)) {
        auto it = m_entries.find(manifest->formatId);
        if (it == m_entries.end()) {
            throw std::runtime_error("FormatRegistry: manifest references unknown format: " + manifest->formatId);
        }
        if (manifest->version != it->second.descriptor.version) {
            throw std::runtime_error("FormatRegistry: manifest version mismatch for format: " + manifest->formatId);
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

std::optional<ProbeResult> FormatRegistry::probeFromManifest(const Entry& entry, const PersistenceContext& context) const {
    if (!context.storage) {
        return std::nullopt;
    }
    auto manifest = readManifest(*context.storage, context);
    if (!manifest) {
        return std::nullopt;
    }
    if (manifest->formatId != entry.descriptor.id || manifest->version != entry.descriptor.version) {
        return std::nullopt;
    }
    return ProbeResult{entry.descriptor.id, entry.descriptor.version, 1.0f};
}

std::optional<ProbeResult> FormatRegistry::probeFromStorage(const Entry& entry, StorageBackend& storage, const PersistenceContext& context) const {
    if (!entry.probe) {
        return std::nullopt;
    }
    return entry.probe(storage, context);
}

} // namespace Rigel::Persistence
