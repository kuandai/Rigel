#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Rigel::Persistence {

using StorageEntryVisitor = std::function<bool(const std::string&)>;

enum class StorageEntryKind {
    Missing,
    RegularFile,
    Directory,
    Other
};

enum class DirectoryPublicationState {
    NotPublished,
    Indeterminate
};

class DirectoryPublicationError : public std::runtime_error {
public:
    DirectoryPublicationError(DirectoryPublicationState state,
                              const std::string& message)
        : std::runtime_error(message)
        , m_state(state) {
    }

    DirectoryPublicationState state() const noexcept {
        return m_state;
    }

private:
    DirectoryPublicationState m_state;
};

class StorageReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ByteReader {
public:
    virtual ~ByteReader() = default;

    virtual uint8_t readU8() = 0;
    virtual uint16_t readU16() = 0;
    virtual uint32_t readU32() = 0;
    virtual int32_t readI32() = 0;
    virtual void readBytes(uint8_t* dst, size_t len) = 0;

    virtual size_t size() const = 0;
    virtual size_t tell() const = 0;
    virtual void seek(size_t offset) = 0;
    virtual std::vector<uint8_t> readAt(size_t offset, size_t len) = 0;
};

class ByteWriter {
public:
    virtual ~ByteWriter() = default;

    virtual void writeU8(uint8_t value) = 0;
    virtual void writeU16(uint16_t value) = 0;
    virtual void writeU32(uint32_t value) = 0;
    virtual void writeI32(int32_t value) = 0;
    virtual void writeBytes(const uint8_t* src, size_t len) = 0;

    virtual size_t size() const = 0;
    virtual size_t tell() const = 0;
    virtual void seek(size_t offset) = 0;
    virtual void writeAt(size_t offset, const uint8_t* src, size_t len) = 0;
    virtual void flush() = 0;
};

class AtomicWriteSession {
public:
    virtual ~AtomicWriteSession() = default;

    virtual ByteWriter& writer() = 0;
    virtual void commit() = 0;
    virtual void abort() = 0;
};

class WorldGenerationBootstrapLock {
public:
    virtual ~WorldGenerationBootstrapLock() = default;
};

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual std::unique_ptr<ByteReader> openRead(const std::string& path) = 0;
    // Writes are staged until commit. Aborting or destroying an uncommitted
    // session leaves the destination unchanged.
    virtual std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path) = 0;
    virtual bool exists(const std::string& path) = 0;
    // Classifies the directory entry itself without following symbolic links.
    // The safe default is Other for backends without that capability.
    virtual StorageEntryKind entryKind(const std::string& path);
    virtual void forEachEntry(const std::string& path,
                              const StorageEntryVisitor& visitor) = 0;
    virtual std::vector<std::string> list(const std::string& path);
    virtual void mkdirs(const std::string& path) = 0;
    // Atomically creates the directory only when no entry already occupies
    // its path. Returns false for a collision. Backends without that
    // guarantee reject the operation.
    virtual bool createDirectoryExclusive(const std::string& path);
    // Creates and durably writes a regular file only when no directory entry
    // already occupies its path. Returns false for a collision. Backends
    // without that guarantee reject the operation.
    virtual bool createFileExclusive(const std::string& path,
                                     const std::string& contents);
    // Serializes bootstrap recovery and initial publication for one world.
    // The lock spans backend instances and cooperating processes. While it is
    // held, backend inspection/removal operations must not themselves replace
    // a bootstrap path between ownership validation and mutation.
    virtual std::unique_ptr<WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& worldRoot);
    virtual void remove(const std::string& path) = 0;
    // Atomically publishes a prepared directory to a sibling path only when
    // the destination does not already exist. A backend may report
    // NotPublished only when no rename occurred; post-rename failures must be
    // Indeterminate. Other exception types are also treated as indeterminate
    // by publication recovery. Backends without the no-replace guarantee
    // reject the operation.
    virtual void publishDirectory(const std::string& stagedPath,
                                  const std::string& finalPath);
};

class FilesystemBackend : public StorageBackend {
public:
    std::unique_ptr<ByteReader> openRead(const std::string& path) override;
    std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path) override;
    bool exists(const std::string& path) override;
    StorageEntryKind entryKind(const std::string& path) override;
    void forEachEntry(const std::string& path,
                      const StorageEntryVisitor& visitor) override;
    void mkdirs(const std::string& path) override;
    bool createDirectoryExclusive(const std::string& path) override;
    bool createFileExclusive(const std::string& path,
                             const std::string& contents) override;
    std::unique_ptr<WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& worldRoot) override;
    void remove(const std::string& path) override;
    void publishDirectory(const std::string& stagedPath,
                          const std::string& finalPath) override;
};

} // namespace Rigel::Persistence
