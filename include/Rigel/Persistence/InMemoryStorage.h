#pragma once

#include "Storage.h"

#include <memory>

namespace Rigel::Persistence {

/**
 * Thread-safe, process-private storage with filesystem-like directory and
 * atomic-publication semantics. Data lives only for the lifetime of this
 * backend instance.
 */
class InMemoryStorageBackend final : public StorageBackend {
public:
    struct State;

    InMemoryStorageBackend();
    ~InMemoryStorageBackend() override;

    InMemoryStorageBackend(const InMemoryStorageBackend&) = delete;
    InMemoryStorageBackend& operator=(const InMemoryStorageBackend&) = delete;

    std::unique_ptr<ByteReader> openRead(const std::string& path) override;
    std::unique_ptr<AtomicWriteSession> openWrite(
        const std::string& path) override;
    bool exists(const std::string& path) override;
    StorageEntryKind entryKind(const std::string& path) override;
    void forEachEntry(
        const std::string& path,
        const StorageEntryVisitor& visitor) override;
    void mkdirs(const std::string& path) override;
    bool createDirectoryExclusive(const std::string& path) override;
    bool createFileExclusive(
        const std::string& path,
        const std::string& contents) override;
    std::unique_ptr<WorldGenerationBootstrapLock>
    lockWorldGenerationBootstrap(const std::string& worldRoot) override;
    void remove(const std::string& path) override;
    void publishDirectory(
        const std::string& stagedPath,
        const std::string& finalPath) override;

private:
    std::shared_ptr<State> m_state;
};

} // namespace Rigel::Persistence
