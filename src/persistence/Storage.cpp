#include "Rigel/Persistence/Storage.h"

#include "AtomicFileCommit.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
            MOVEFILE_REPLACE_EXISTING) == 0) {
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return;
    }
    error.clear();
#else
    std::filesystem::rename(tempPath, finalPath, error);
#endif
}

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
            [](const std::filesystem::path& tempPath,
               const std::filesystem::path& finalPath,
               std::error_code& error) {
                replaceFileAtomically(tempPath, finalPath, error);
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
    std::filesystem::create_directories(p.parent_path());

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

std::vector<std::string> FilesystemBackend::list(const std::string& path) {
    std::vector<std::string> result;
    if (!std::filesystem::exists(path)) {
        return result;
    }
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        result.push_back(entry.path().string());
    }
    return result;
}

void FilesystemBackend::mkdirs(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::filesystem::create_directories(path);
}

void FilesystemBackend::remove(const std::string& path) {
    std::filesystem::remove(path);
}

} // namespace Rigel::Persistence
