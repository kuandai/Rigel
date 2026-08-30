#include "Rigel/Persistence/InMemoryStorage.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Rigel::Persistence {
namespace {

constexpr size_t kMaximumInMemoryFileBytes = 256 * 1024 * 1024;

std::string normalizedPath(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("In-memory storage path must not be empty");
    }
    return std::filesystem::path(path).lexically_normal().generic_string();
}

std::string parentPath(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return ".";
    }
    return parent.generic_string();
}

bool isRootPath(const std::string& path) {
    return path == "." || path == "/";
}

bool isDirectChild(const std::string& parent, const std::string& child) {
    return parentPath(child) == parent && child != parent;
}

bool isDescendantOrSelf(const std::string& root, const std::string& path) {
    if (path == root) {
        return true;
    }
    const std::string prefix = root == "/" ? root : root + "/";
    return path.starts_with(prefix);
}

class VectorByteReader final : public ByteReader {
public:
    VectorByteReader(std::string path, std::vector<uint8_t> bytes)
        : m_path(std::move(path)), m_bytes(std::move(bytes)) {}

    uint8_t readU8() override {
        requireRange(m_position, 1);
        return m_bytes[m_position++];
    }

    uint16_t readU16() override {
        uint16_t value = static_cast<uint16_t>(readU8()) << 8;
        value |= static_cast<uint16_t>(readU8());
        return value;
    }

    uint32_t readU32() override {
        uint32_t value = static_cast<uint32_t>(readU8()) << 24;
        value |= static_cast<uint32_t>(readU8()) << 16;
        value |= static_cast<uint32_t>(readU8()) << 8;
        value |= static_cast<uint32_t>(readU8());
        return value;
    }

    int32_t readI32() override {
        return static_cast<int32_t>(readU32());
    }

    void readBytes(uint8_t* destination, size_t length) override {
        requireRange(m_position, length);
        if (length != 0) {
            std::copy_n(
                m_bytes.data() + m_position, length, destination);
        }
        m_position += length;
    }

    size_t size() const override { return m_bytes.size(); }
    size_t tell() const override { return m_position; }

    void seek(size_t offset) override {
        requireRange(offset, 0);
        m_position = offset;
    }

    std::vector<uint8_t> readAt(size_t offset, size_t length) override {
        requireRange(offset, length);
        return std::vector<uint8_t>(
            m_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            m_bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }

private:
    void requireRange(size_t offset, size_t length) const {
        if (offset > m_bytes.size() || length > m_bytes.size() - offset) {
            throw StorageReadError(
                "Unexpected end of in-memory file while reading: " + m_path);
        }
    }

    std::string m_path;
    std::vector<uint8_t> m_bytes;
    size_t m_position = 0;
};

class VectorByteWriter final : public ByteWriter {
public:
    explicit VectorByteWriter(std::vector<uint8_t>& bytes)
        : m_bytes(bytes) {}

    void writeU8(uint8_t value) override {
        writeBytes(&value, 1);
    }

    void writeU16(uint16_t value) override {
        writeU8(static_cast<uint8_t>((value >> 8) & 0xff));
        writeU8(static_cast<uint8_t>(value & 0xff));
    }

    void writeU32(uint32_t value) override {
        writeU8(static_cast<uint8_t>((value >> 24) & 0xff));
        writeU8(static_cast<uint8_t>((value >> 16) & 0xff));
        writeU8(static_cast<uint8_t>((value >> 8) & 0xff));
        writeU8(static_cast<uint8_t>(value & 0xff));
    }

    void writeI32(int32_t value) override {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeBytes(const uint8_t* source, size_t length) override {
        requireCapacity(m_position, length);
        if (m_position + length > m_bytes.size()) {
            m_bytes.resize(m_position + length, 0);
        }
        if (length != 0) {
            std::copy_n(source, length, m_bytes.data() + m_position);
        }
        m_position += length;
    }

    size_t size() const override { return m_bytes.size(); }
    size_t tell() const override { return m_position; }

    void seek(size_t offset) override {
        requireCapacity(offset, 0);
        if (offset > m_bytes.size()) {
            m_bytes.resize(offset, 0);
        }
        m_position = offset;
    }

    void writeAt(
        size_t offset,
        const uint8_t* source,
        size_t length) override {
        requireCapacity(offset, length);
        if (offset + length > m_bytes.size()) {
            m_bytes.resize(offset + length, 0);
        }
        if (length != 0) {
            std::copy_n(source, length, m_bytes.data() + offset);
        }
    }

    void flush() override {}

private:
    static void requireCapacity(size_t offset, size_t length) {
        if (offset > kMaximumInMemoryFileBytes ||
            length > kMaximumInMemoryFileBytes - offset) {
            throw std::length_error(
                "In-memory storage file exceeds the supported size limit");
        }
    }

    std::vector<uint8_t>& m_bytes;
    size_t m_position = 0;
};

} // namespace

struct InMemoryStorageBackend::State {
    struct Entry {
        StorageEntryKind kind = StorageEntryKind::Missing;
        std::vector<uint8_t> bytes;
    };

    mutable std::shared_mutex entriesMutex;
    std::map<std::string, Entry> entries;
    std::mutex bootstrapMutexesMutex;
    std::map<std::string, std::shared_ptr<std::mutex>> bootstrapMutexes;

    StorageEntryKind entryKindLocked(const std::string& path) const;
    void makeDirectoriesLocked(const std::string& requestedPath);

    class AtomicWriteSession;
};

StorageEntryKind InMemoryStorageBackend::State::entryKindLocked(
    const std::string& path) const {
    if (isRootPath(path)) {
        return StorageEntryKind::Directory;
    }
    const auto found = entries.find(path);
    return found == entries.end()
        ? StorageEntryKind::Missing
        : found->second.kind;
}

void InMemoryStorageBackend::State::makeDirectoriesLocked(
    const std::string& requestedPath) {
    std::string current = requestedPath;
    std::vector<std::string> missing;
    while (!isRootPath(current)) {
        const StorageEntryKind kind = entryKindLocked(current);
        if (kind == StorageEntryKind::Directory) {
            break;
        }
        if (kind != StorageEntryKind::Missing) {
            throw std::runtime_error(
                "In-memory storage directory path is occupied: " + current);
        }
        missing.push_back(current);
        const std::string parent = parentPath(current);
        if (parent == current) {
            throw std::runtime_error(
                "In-memory storage could not resolve directory parent: " +
                current);
        }
        current = parent;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        entries.emplace(
            *it,
            Entry{StorageEntryKind::Directory, {}});
    }
}

class InMemoryStorageBackend::State::AtomicWriteSession final
    : public Rigel::Persistence::AtomicWriteSession {
public:
    AtomicWriteSession(
        std::shared_ptr<State> state,
        std::string path)
        : m_state(std::move(state))
        , m_path(std::move(path))
        , m_writer(m_bytes) {}

    ByteWriter& writer() override {
        requireActive();
        return m_writer;
    }

    void commit() override {
        requireActive();
        std::unique_lock lock(m_state->entriesMutex);
        m_state->makeDirectoriesLocked(parentPath(m_path));
        if (m_state->entryKindLocked(m_path) ==
            StorageEntryKind::Directory) {
            throw AtomicFilePublicationError(
                AtomicFilePublicationState::NotPublished,
                "Cannot replace an in-memory directory with a file: " +
                    m_path);
        }
        auto replacement = m_state->entries;
        replacement[m_path] = State::Entry{
            StorageEntryKind::RegularFile, std::move(m_bytes)};
        m_state->entries.swap(replacement);
        m_active = false;
    }

    void abort() override {
        m_bytes.clear();
        m_active = false;
    }

private:
    void requireActive() const {
        if (!m_active) {
            throw std::logic_error(
                "In-memory atomic write session is no longer active");
        }
    }

    std::shared_ptr<State> m_state;
    std::string m_path;
    std::vector<uint8_t> m_bytes;
    VectorByteWriter m_writer;
    bool m_active = true;
};

namespace {

class InMemoryBootstrapLock final : public WorldGenerationBootstrapLock {
public:
    explicit InMemoryBootstrapLock(std::shared_ptr<std::mutex> mutex)
        : m_mutex(std::move(mutex)), m_lock(*m_mutex) {}

private:
    std::shared_ptr<std::mutex> m_mutex;
    std::unique_lock<std::mutex> m_lock;
};

} // namespace

InMemoryStorageBackend::InMemoryStorageBackend()
    : m_state(std::make_shared<State>()) {}

InMemoryStorageBackend::~InMemoryStorageBackend() = default;

std::unique_ptr<ByteReader> InMemoryStorageBackend::openRead(
    const std::string& path) {
    const std::string normalized = normalizedPath(path);
    std::shared_lock lock(m_state->entriesMutex);
    const auto found = m_state->entries.find(normalized);
    if (found == m_state->entries.end() ||
        found->second.kind != StorageEntryKind::RegularFile) {
        throw StorageReadError(
            "Failed to open in-memory file for reading: " + normalized);
    }
    return std::make_unique<VectorByteReader>(
        normalized, found->second.bytes);
}

std::unique_ptr<AtomicWriteSession> InMemoryStorageBackend::openWrite(
    const std::string& path) {
    const std::string normalized = normalizedPath(path);
    {
        std::unique_lock lock(m_state->entriesMutex);
        m_state->makeDirectoriesLocked(parentPath(normalized));
        if (m_state->entryKindLocked(normalized) ==
            StorageEntryKind::Directory) {
            throw std::runtime_error(
                "Cannot open an in-memory directory for writing: " +
                normalized);
        }
    }
    return std::make_unique<State::AtomicWriteSession>(m_state, normalized);
}

bool InMemoryStorageBackend::exists(const std::string& path) {
    return entryKind(path) != StorageEntryKind::Missing;
}

StorageEntryKind InMemoryStorageBackend::entryKind(const std::string& path) {
    const std::string normalized = normalizedPath(path);
    std::shared_lock lock(m_state->entriesMutex);
    return m_state->entryKindLocked(normalized);
}

void InMemoryStorageBackend::forEachEntry(
    const std::string& path,
    const StorageEntryVisitor& visitor) {
    const std::string normalized = normalizedPath(path);
    std::vector<std::string> children;
    {
        std::shared_lock lock(m_state->entriesMutex);
        if (m_state->entryKindLocked(normalized) !=
            StorageEntryKind::Directory) {
            return;
        }
        for (const auto& [candidate, entry] : m_state->entries) {
            static_cast<void>(entry);
            if (isDirectChild(normalized, candidate)) {
                children.push_back(candidate);
            }
        }
    }
    for (const std::string& child : children) {
        if (!visitor(child)) {
            return;
        }
    }
}

void InMemoryStorageBackend::mkdirs(const std::string& path) {
    const std::string normalized = normalizedPath(path);
    std::unique_lock lock(m_state->entriesMutex);
    m_state->makeDirectoriesLocked(normalized);
}

bool InMemoryStorageBackend::createDirectoryExclusive(
    const std::string& path) {
    const std::string normalized = normalizedPath(path);
    if (isRootPath(normalized)) {
        return false;
    }
    std::unique_lock lock(m_state->entriesMutex);
    m_state->makeDirectoriesLocked(parentPath(normalized));
    if (m_state->entryKindLocked(normalized) != StorageEntryKind::Missing) {
        return false;
    }
    m_state->entries.emplace(
        normalized,
        State::Entry{StorageEntryKind::Directory, {}});
    return true;
}

bool InMemoryStorageBackend::createFileExclusive(
    const std::string& path,
    const std::string& contents) {
    const std::string normalized = normalizedPath(path);
    if (contents.size() > kMaximumInMemoryFileBytes) {
        throw std::length_error(
            "In-memory storage file exceeds the supported size limit");
    }
    std::unique_lock lock(m_state->entriesMutex);
    m_state->makeDirectoriesLocked(parentPath(normalized));
    if (m_state->entryKindLocked(normalized) != StorageEntryKind::Missing) {
        return false;
    }
    m_state->entries.emplace(
        normalized,
        State::Entry{
            StorageEntryKind::RegularFile,
            std::vector<uint8_t>(contents.begin(), contents.end())});
    return true;
}

std::unique_ptr<WorldGenerationBootstrapLock>
InMemoryStorageBackend::lockWorldGenerationBootstrap(
    const std::string& worldRoot) {
    const std::string normalized = normalizedPath(worldRoot);
    std::shared_ptr<std::mutex> mutex;
    {
        std::lock_guard lock(m_state->bootstrapMutexesMutex);
        auto& stored = m_state->bootstrapMutexes[normalized];
        if (!stored) {
            stored = std::make_shared<std::mutex>();
        }
        mutex = stored;
    }
    return std::make_unique<InMemoryBootstrapLock>(std::move(mutex));
}

void InMemoryStorageBackend::remove(const std::string& path) {
    const std::string normalized = normalizedPath(path);
    if (isRootPath(normalized)) {
        throw std::runtime_error(
            "Cannot remove an in-memory storage root");
    }
    std::unique_lock lock(m_state->entriesMutex);
    const auto found = m_state->entries.find(normalized);
    if (found == m_state->entries.end()) {
        return;
    }
    if (found->second.kind == StorageEntryKind::Directory) {
        const bool hasChildren = std::any_of(
            m_state->entries.begin(),
            m_state->entries.end(),
            [&](const auto& entry) {
                return entry.first != normalized &&
                    isDescendantOrSelf(normalized, entry.first);
            });
        if (hasChildren) {
            throw std::runtime_error(
                "Cannot remove a non-empty in-memory directory: " +
                normalized);
        }
    }
    m_state->entries.erase(found);
}

void InMemoryStorageBackend::publishDirectory(
    const std::string& stagedPath,
    const std::string& finalPath) {
    const std::string staged = normalizedPath(stagedPath);
    const std::string final = normalizedPath(finalPath);
    if (parentPath(staged) != parentPath(final)) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "In-memory directory publication requires sibling paths");
    }

    std::unique_lock lock(m_state->entriesMutex);
    if (m_state->entryKindLocked(staged) != StorageEntryKind::Directory) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "In-memory staged publication is not a directory: " + staged);
    }
    if (m_state->entryKindLocked(final) != StorageEntryKind::Missing) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "In-memory publication destination already exists: " + final);
    }

    auto replacement = m_state->entries;
    std::vector<std::pair<std::string, State::Entry>> moved;
    for (auto it = replacement.begin(); it != replacement.end();) {
        if (!isDescendantOrSelf(staged, it->first)) {
            ++it;
            continue;
        }
        const std::string suffix = it->first.substr(staged.size());
        moved.emplace_back(final + suffix, std::move(it->second));
        it = replacement.erase(it);
    }
    for (auto& [path, entry] : moved) {
        if (!replacement.emplace(path, std::move(entry)).second) {
            throw DirectoryPublicationError(
                DirectoryPublicationState::NotPublished,
                "In-memory directory publication would replace an entry: " +
                    path);
        }
    }
    m_state->entries.swap(replacement);
}

} // namespace Rigel::Persistence
