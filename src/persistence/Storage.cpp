#include "Rigel/Persistence/Storage.h"

#include "AtomicFileCommit.h"
#include "DurableDirectory.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <stdio.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

namespace Rigel::Persistence {

namespace {

std::filesystem::path createUniqueTemporaryFile(const std::filesystem::path& finalPath) {
    static std::atomic<uint64_t> nextId{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

    for (;;) {
        auto candidate = std::filesystem::path(
            finalPath.string() + ".tmp." + std::to_string(timestamp) + "." +
            std::to_string(nextId.fetch_add(1, std::memory_order_relaxed)));

        errno = 0;
        auto* file = std::fopen(candidate.string().c_str(), "wbx");
        if (file == nullptr) {
            if (errno == EEXIST) {
                continue;
            }
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to create temporary file for " + finalPath.string());
        }

        if (std::fclose(file) != 0) {
            const int closeError = errno == 0 ? EIO : errno;
            std::error_code cleanupError;
            std::filesystem::remove(candidate, cleanupError);
            throw std::system_error(
                closeError,
                std::generic_category(),
                "Failed to close temporary file for " + finalPath.string());
        }
        return candidate;
    }
}

void replaceFileAtomically(const std::filesystem::path& tempPath,
                           const std::filesystem::path& finalPath,
                           std::error_code& error) {
#ifdef _WIN32
    if (::MoveFileExW(
            tempPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return;
    }
    error.clear();
#else
    std::filesystem::rename(tempPath, finalPath, error);
#endif
}

#ifdef _WIN32

void synchronizeFile(const std::filesystem::path& path) {
    const HANDLE file = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(::GetLastError()),
            std::system_category(),
            "Failed to open file for synchronization: " + path.string());
    }
    if (::FlushFileBuffers(file) == 0) {
        const auto error = static_cast<int>(::GetLastError());
        ::CloseHandle(file);
        throw std::system_error(
            error,
            std::system_category(),
            "Failed to synchronize file: " + path.string());
    }
    if (::CloseHandle(file) == 0) {
        throw std::system_error(
            static_cast<int>(::GetLastError()),
            std::system_category(),
            "Failed to close synchronized file: " + path.string());
    }
}

void synchronizeDirectory(const std::filesystem::path&) {
}

#else

void synchronizeDescriptor(int descriptor,
                           const std::filesystem::path& path,
                           const char* description) {
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(
            error,
            std::generic_category(),
            std::string("Failed to synchronize ") + description + ": " +
                path.string());
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            std::string("Failed to close synchronized ") + description +
                ": " + path.string());
    }
}

void synchronizeFile(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to open file for synchronization: " + path.string());
    }
    synchronizeDescriptor(descriptor, path, "file");
}

void synchronizeDirectory(const std::filesystem::path& path) {
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to open directory for synchronization: " + path.string());
    }
    synchronizeDescriptor(descriptor, path, "directory");
}

#endif

void prepareDirectories(const std::filesystem::path& path) {
    detail::createDirectoriesDurably(
        path,
        [](const std::filesystem::path& directoryPath) {
            std::filesystem::create_directory(directoryPath);
        },
        [](const std::filesystem::path& directoryPath) {
            synchronizeDirectory(directoryPath);
        });
}

std::filesystem::path worldGenerationLockPath(
    const std::filesystem::path& worldRoot) {
    const std::filesystem::path absoluteRoot =
        std::filesystem::absolute(worldRoot).lexically_normal();
    return std::filesystem::path(
        absoluteRoot.string() + ".rigel-bootstrap.lock");
}

#ifdef _WIN32

class FilesystemWorldGenerationBootstrapLock final
    : public WorldGenerationBootstrapLock {
public:
    explicit FilesystemWorldGenerationBootstrapLock(
        const std::filesystem::path& worldRoot) {
        const std::filesystem::path lockPath =
            worldGenerationLockPath(worldRoot);
        prepareDirectories(lockPath.parent_path());
        m_file = ::CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            throw std::system_error(
                static_cast<int>(::GetLastError()),
                std::system_category(),
                "Failed to open world bootstrap lock: " +
                    lockPath.string());
        }

        BY_HANDLE_FILE_INFORMATION information{};
        const bool informationRead =
            ::GetFileInformationByHandle(m_file, &information) != 0;
        if (!informationRead ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) !=
                0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const int error = informationRead
                ? ERROR_INVALID_DATA
                : static_cast<int>(::GetLastError());
            ::CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
            throw std::system_error(
                error,
                std::system_category(),
                "World bootstrap lock is not a regular file: " +
                    lockPath.string());
        }

        OVERLAPPED overlapped{};
        if (::LockFileEx(
                m_file,
                LOCKFILE_EXCLUSIVE_LOCK,
                0,
                1,
                0,
                &overlapped) == 0) {
            const int error = static_cast<int>(::GetLastError());
            ::CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
            throw std::system_error(
                error,
                std::system_category(),
                "Failed to acquire world bootstrap lock: " +
                    lockPath.string());
        }
    }

    ~FilesystemWorldGenerationBootstrapLock() override {
        if (m_file == INVALID_HANDLE_VALUE) {
            return;
        }
        OVERLAPPED overlapped{};
        ::UnlockFileEx(m_file, 0, 1, 0, &overlapped);
        ::CloseHandle(m_file);
    }

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
};

#else

class FilesystemWorldGenerationBootstrapLock final
    : public WorldGenerationBootstrapLock {
public:
    explicit FilesystemWorldGenerationBootstrapLock(
        const std::filesystem::path& worldRoot) {
        const std::filesystem::path lockPath =
            worldGenerationLockPath(worldRoot);
        prepareDirectories(lockPath.parent_path());
        m_descriptor = ::open(
            lockPath.c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            0600);
        if (m_descriptor < 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to open world bootstrap lock: " +
                    lockPath.string());
        }

        struct stat status {};
        const bool statusRead = ::fstat(m_descriptor, &status) == 0;
        if (!statusRead || !S_ISREG(status.st_mode)) {
            const int error = statusRead ? EINVAL : errno;
            ::close(m_descriptor);
            m_descriptor = -1;
            throw std::system_error(
                error,
                std::generic_category(),
                "World bootstrap lock is not a regular file: " +
                    lockPath.string());
        }

        while (::flock(m_descriptor, LOCK_EX) != 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            ::close(m_descriptor);
            m_descriptor = -1;
            throw std::system_error(
                error,
                std::generic_category(),
                "Failed to acquire world bootstrap lock: " +
                    lockPath.string());
        }
    }

    ~FilesystemWorldGenerationBootstrapLock() override {
        if (m_descriptor < 0) {
            return;
        }
        ::flock(m_descriptor, LOCK_UN);
        ::close(m_descriptor);
    }

private:
    int m_descriptor = -1;
};

#endif

class FileByteReader final : public ByteReader {
public:
    explicit FileByteReader(const std::string& path)
        : m_path(path), m_stream(path, std::ios::binary), m_size(0) {
        if (!m_stream.is_open()) {
            throw StorageReadError("Failed to open file for reading: " + path);
        }
        m_size = static_cast<size_t>(std::filesystem::file_size(path));
    }

    uint8_t readU8() override {
        requireAvailable(1);
        char value = 0;
        m_stream.read(&value, 1);
        if (!m_stream) {
            throw StorageReadError("Failed to read byte from: " + m_path);
        }
        return static_cast<uint8_t>(value);
    }

    uint16_t readU16() override {
        uint16_t value = 0;
        value |= static_cast<uint16_t>(readU8()) << 8;
        value |= static_cast<uint16_t>(readU8());
        return value;
    }

    uint32_t readU32() override {
        uint32_t value = 0;
        value |= static_cast<uint32_t>(readU8()) << 24;
        value |= static_cast<uint32_t>(readU8()) << 16;
        value |= static_cast<uint32_t>(readU8()) << 8;
        value |= static_cast<uint32_t>(readU8());
        return value;
    }

    int32_t readI32() override {
        return static_cast<int32_t>(readU32());
    }

    void readBytes(uint8_t* dst, size_t len) override {
        if (len == 0) {
            return;
        }
        requireAvailable(len);
        m_stream.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
        if (!m_stream) {
            throw StorageReadError("Failed to read bytes from: " + m_path);
        }
    }

    size_t size() const override {
        return m_size;
    }

    size_t tell() const override {
        return static_cast<size_t>(m_stream.tellg());
    }

    void seek(size_t offset) override {
        m_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    }

    std::vector<uint8_t> readAt(size_t offset, size_t len) override {
        requireRange(offset, len);
        std::vector<uint8_t> out(len);
        if (len == 0) {
            return out;
        }

        auto current = tell();
        try {
            seek(offset);
            readBytes(out.data(), len);
        } catch (...) {
            m_stream.clear();
            seek(current);
            throw;
        }
        seek(current);
        return out;
    }

private:
    void requireAvailable(size_t len) {
        const auto position = m_stream.tellg();
        if (position == std::streampos(-1)) {
            if (m_stream.bad()) {
                throw StorageReadError("Failed to determine read position in: " + m_path);
            }
            throw std::runtime_error("Invalid read position in: " + m_path);
        }

        const size_t offset = static_cast<size_t>(position);
        requireRange(offset, len);
    }

    void requireRange(size_t offset, size_t len) const {
        if (offset > m_size || len > m_size - offset) {
            throw std::runtime_error("Unexpected end of file while reading: " + m_path);
        }
    }

    std::string m_path;
    mutable std::ifstream m_stream;
    size_t m_size;
};

class FileByteWriter final : public ByteWriter {
public:
    explicit FileByteWriter(const std::string& path)
        : m_path(path), m_stream(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc) {
        if (!m_stream.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + path);
        }
    }

    void writeU8(uint8_t value) override {
        char b = static_cast<char>(value);
        m_stream.write(&b, 1);
        if (!m_stream) {
            throw std::runtime_error("Failed to write byte to: " + m_path);
        }
    }

    void writeU16(uint16_t value) override {
        writeU8(static_cast<uint8_t>((value >> 8) & 0xFF));
        writeU8(static_cast<uint8_t>(value & 0xFF));
    }

    void writeU32(uint32_t value) override {
        writeU8(static_cast<uint8_t>((value >> 24) & 0xFF));
        writeU8(static_cast<uint8_t>((value >> 16) & 0xFF));
        writeU8(static_cast<uint8_t>((value >> 8) & 0xFF));
        writeU8(static_cast<uint8_t>(value & 0xFF));
    }

    void writeI32(int32_t value) override {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeBytes(const uint8_t* src, size_t len) override {
        if (len == 0) {
            return;
        }
        m_stream.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(len));
        if (!m_stream) {
            throw std::runtime_error("Failed to write bytes to: " + m_path);
        }
    }

    size_t size() const override {
        auto current = m_stream.tellp();
        m_stream.seekp(0, std::ios::end);
        auto end = m_stream.tellp();
        m_stream.seekp(current);
        return static_cast<size_t>(end);
    }

    size_t tell() const override {
        return static_cast<size_t>(m_stream.tellp());
    }

    void seek(size_t offset) override {
        m_stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    }

    void writeAt(size_t offset, const uint8_t* src, size_t len) override {
        auto current = tell();
        seek(offset);
        writeBytes(src, len);
        seek(current);
    }

    void flush() override {
        m_stream.flush();
        if (!m_stream) {
            throw std::runtime_error("Failed to flush file: " + m_path);
        }
    }

    void close() {
        m_stream.flush();
        const bool flushFailed = !m_stream;
        if (flushFailed) {
            m_stream.clear();
        }

        m_stream.close();
        if (flushFailed) {
            throw std::runtime_error("Failed to flush file: " + m_path);
        }
        if (!m_stream) {
            throw std::runtime_error("Failed to close file: " + m_path);
        }
    }

private:
    std::string m_path;
    mutable std::fstream m_stream;
};

class AtomicFileWriteSession final : public AtomicWriteSession {
public:
    AtomicFileWriteSession(std::string finalPath, std::string tempPath)
        : m_finalPath(std::move(finalPath)),
          m_tempPath(std::move(tempPath)),
          m_writer(std::make_unique<FileByteWriter>(m_tempPath)) {
    }

    ~AtomicFileWriteSession() override {
        abort();
    }

    ByteWriter& writer() override {
        return *m_writer;
    }

    void commit() override {
        detail::commitAtomicFile(
            m_tempPath,
            m_finalPath,
            [this]() {
                auto writer = std::move(m_writer);
                writer->close();
            },
            [](const std::filesystem::path& tempPath) {
                synchronizeFile(tempPath);
            },
            [](const std::filesystem::path& tempPath,
               const std::filesystem::path& finalPath,
               std::error_code& error) {
                replaceFileAtomically(tempPath, finalPath, error);
            },
            [](const std::filesystem::path& directoryPath) {
                synchronizeDirectory(directoryPath);
            });
    }

    void abort() override {
        if (!m_writer) {
            return;
        }

        m_writer.reset();
        std::error_code ec;
        std::filesystem::remove(m_tempPath, ec);
    }

private:
    std::string m_finalPath;
    std::string m_tempPath;
    std::unique_ptr<FileByteWriter> m_writer;
};

} // namespace

std::unique_ptr<ByteReader> FilesystemBackend::openRead(const std::string& path) {
    return std::make_unique<FileByteReader>(path);
}

std::unique_ptr<AtomicWriteSession> FilesystemBackend::openWrite(const std::string& path) {
    std::filesystem::path p(path);
    prepareDirectories(p.parent_path());

    const auto tempPath = createUniqueTemporaryFile(p);
    try {
        return std::make_unique<AtomicFileWriteSession>(path, tempPath.string());
    } catch (...) {
        std::error_code cleanupError;
        std::filesystem::remove(tempPath, cleanupError);
        throw;
    }
}

bool FilesystemBackend::exists(const std::string& path) {
    return std::filesystem::exists(path);
}

StorageEntryKind StorageBackend::entryKind(const std::string&) {
    return StorageEntryKind::Other;
}

StorageEntryKind FilesystemBackend::entryKind(const std::string& path) {
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path);
    if (status.type() == std::filesystem::file_type::not_found) {
        return StorageEntryKind::Missing;
    }
    if (std::filesystem::is_regular_file(status)) {
        return StorageEntryKind::RegularFile;
    }
    if (std::filesystem::is_directory(status)) {
        return StorageEntryKind::Directory;
    }
    return StorageEntryKind::Other;
}

std::vector<std::string> StorageBackend::list(const std::string& path) {
    std::vector<std::string> entries;
    forEachEntry(path, [&](const std::string& entry) {
        entries.push_back(entry);
        return true;
    });
    return entries;
}

void FilesystemBackend::forEachEntry(
    const std::string& path,
    const StorageEntryVisitor& visitor) {
    if (!std::filesystem::exists(path)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        const std::string entryPath = entry.path().string();
        if (!visitor(entryPath)) {
            return;
        }
    }
}

void FilesystemBackend::mkdirs(const std::string& path) {
    prepareDirectories(std::filesystem::path(path));
}

bool StorageBackend::createDirectoryExclusive(const std::string&) {
    throw std::runtime_error(
        "Storage backend does not support exclusive directory creation");
}

bool FilesystemBackend::createDirectoryExclusive(const std::string& path) {
    const std::filesystem::path directory(path);
    prepareDirectories(directory.parent_path());

    std::error_code error;
    const bool created = std::filesystem::create_directory(directory, error);
    if (!created) {
        if (!error || error == std::errc::file_exists) {
            return false;
        }
        throw std::system_error(
            error,
            "Failed to exclusively create directory: " + directory.string());
    }

    synchronizeDirectory(detail::containingDirectory(directory));
    return true;
}

bool StorageBackend::createFileExclusive(const std::string&,
                                         const std::string&) {
    throw std::runtime_error(
        "Storage backend does not support exclusive file creation");
}

bool FilesystemBackend::createFileExclusive(
    const std::string& path,
    const std::string& contents) {
    const std::filesystem::path filePath(path);
    prepareDirectories(filePath.parent_path());

#ifdef _WIN32
    HANDLE file = ::CreateFileW(
        filePath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const int error = static_cast<int>(::GetLastError());
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return false;
        }
        throw std::system_error(
            error,
            std::system_category(),
            "Failed to exclusively create file: " + filePath.string());
    }

    try {
        size_t offset = 0;
        while (offset < contents.size()) {
            const DWORD remaining = static_cast<DWORD>(std::min<size_t>(
                contents.size() - offset,
                std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (::WriteFile(
                    file,
                    contents.data() + offset,
                    remaining,
                    &written,
                    nullptr) == 0 ||
                written == 0) {
                throw std::system_error(
                    static_cast<int>(::GetLastError()),
                    std::system_category(),
                    "Failed to write exclusively created file: " +
                        filePath.string());
            }
            offset += written;
        }
        if (::FlushFileBuffers(file) == 0) {
            throw std::system_error(
                static_cast<int>(::GetLastError()),
                std::system_category(),
                "Failed to synchronize exclusively created file: " +
                    filePath.string());
        }
        if (::CloseHandle(file) == 0) {
            file = INVALID_HANDLE_VALUE;
            throw std::system_error(
                static_cast<int>(::GetLastError()),
                std::system_category(),
                "Failed to close exclusively created file: " +
                    filePath.string());
        }
        file = INVALID_HANDLE_VALUE;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file);
        }
        ::DeleteFileW(filePath.c_str());
        throw;
    }
#else
    int descriptor = ::open(
        filePath.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return false;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to exclusively create file: " + filePath.string());
    }

    try {
        size_t offset = 0;
        while (offset < contents.size()) {
            const ssize_t written = ::write(
                descriptor,
                contents.data() + offset,
                contents.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::system_error(
                    errno == 0 ? EIO : errno,
                    std::generic_category(),
                    "Failed to write exclusively created file: " +
                        filePath.string());
            }
            offset += static_cast<size_t>(written);
        }
        if (::fsync(descriptor) != 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to synchronize exclusively created file: " +
                    filePath.string());
        }
        if (::close(descriptor) != 0) {
            descriptor = -1;
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to close exclusively created file: " +
                    filePath.string());
        }
        descriptor = -1;
    } catch (...) {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        std::error_code cleanupError;
        std::filesystem::remove(filePath, cleanupError);
        throw;
    }
#endif

    synchronizeDirectory(detail::containingDirectory(filePath));
    return true;
}

std::unique_ptr<WorldGenerationBootstrapLock>
StorageBackend::lockWorldGenerationBootstrap(const std::string&) {
    throw std::runtime_error(
        "Storage backend does not support world bootstrap locking");
}

std::unique_ptr<WorldGenerationBootstrapLock>
FilesystemBackend::lockWorldGenerationBootstrap(
    const std::string& worldRoot) {
    return std::make_unique<FilesystemWorldGenerationBootstrapLock>(
        worldRoot);
}

void FilesystemBackend::remove(const std::string& path) {
    detail::removeFileDurably(
        std::filesystem::path(path),
        [](const std::filesystem::path& removalPath) {
            return std::filesystem::remove(removalPath);
        },
        [](const std::filesystem::path& directoryPath) {
            synchronizeDirectory(directoryPath);
        });
}

void StorageBackend::publishDirectory(const std::string&,
                                      const std::string&) {
    throw DirectoryPublicationError(
        DirectoryPublicationState::NotPublished,
        "Storage backend does not support atomic directory publication");
}

void FilesystemBackend::publishDirectory(const std::string& stagedPath,
                                         const std::string& finalPath) {
    const std::filesystem::path staged(stagedPath);
    const std::filesystem::path final(finalPath);
    const std::filesystem::path stagedParent =
        detail::containingDirectory(staged);
    const std::filesystem::path parent = detail::containingDirectory(final);
    if (stagedParent.lexically_normal() != parent.lexically_normal()) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "Atomic directory publication requires sibling staging and final paths");
    }
    try {
        prepareDirectories(parent);
    } catch (const std::exception& failure) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "Failed to prepare directory publication parent: " +
                parent.string() + ": " + failure.what());
    }

#ifdef _WIN32
    if (::MoveFileExW(
            staged.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
        const std::error_code error(
            static_cast<int>(::GetLastError()), std::system_category());
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "Failed to publish directory without replacing existing save: " +
                final.string() + ": " + error.message());
    }
#elif defined(__linux__)
    if (::syscall(
            SYS_renameat2,
            AT_FDCWD,
            staged.c_str(),
            AT_FDCWD,
            final.c_str(),
            1U) != 0) {
        const std::error_code error(errno, std::generic_category());
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "Failed to publish directory without replacing existing save: " +
                final.string() + ": " + error.message());
    }
    try {
        synchronizeDirectory(parent);
    } catch (const std::exception& failure) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::Indeterminate,
            "Published directory but failed to synchronize its parent: " +
                final.string() + ": " + failure.what());
    }
#elif defined(__APPLE__)
    if (::renamex_np(staged.c_str(), final.c_str(), RENAME_EXCL) != 0) {
        const std::error_code error(errno, std::generic_category());
        throw DirectoryPublicationError(
            DirectoryPublicationState::NotPublished,
            "Failed to publish directory without replacing existing save: " +
                final.string() + ": " + error.message());
    }
    try {
        synchronizeDirectory(parent);
    } catch (const std::exception& failure) {
        throw DirectoryPublicationError(
            DirectoryPublicationState::Indeterminate,
            "Published directory but failed to synchronize its parent: " +
                final.string() + ": " + failure.what());
    }
#else
    throw DirectoryPublicationError(
        DirectoryPublicationState::NotPublished,
        "Atomic no-replace directory publication is unsupported on this platform");
#endif
}

} // namespace Rigel::Persistence
