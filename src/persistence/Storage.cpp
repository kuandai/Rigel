#include "Rigel/Persistence/Storage.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace Rigel::Persistence {

namespace {

class FileByteReader final : public ByteReader {
public:
    explicit FileByteReader(const std::string& path)
        : m_path(path), m_stream(path, std::ios::binary), m_size(0) {
        if (!m_stream.is_open()) {
            throw std::runtime_error("Failed to open file for reading: " + path);
        }
        m_size = static_cast<size_t>(std::filesystem::file_size(path));
    }

    uint8_t readU8() override {
        char value = 0;
        m_stream.read(&value, 1);
        if (!m_stream) {
            throw std::runtime_error("Failed to read byte from: " + m_path);
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
        m_stream.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
        if (!m_stream) {
            throw std::runtime_error("Failed to read bytes from: " + m_path);
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
        auto current = tell();
        seek(offset);
        std::vector<uint8_t> out(len);
        if (len > 0) {
            readBytes(out.data(), len);
        }
        seek(current);
        return out;
    }

private:
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
    }

private:
    std::string m_path;
    mutable std::fstream m_stream;
};

class AtomicFileWriteSession final : public AtomicWriteSession {
public:
    AtomicFileWriteSession(std::string finalPath, std::string tempPath)
        : m_finalPath(std::move(finalPath)), m_tempPath(std::move(tempPath)), m_writer(m_tempPath) {
    }

    ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
        m_writer.flush();
        std::error_code ec;
        if (std::filesystem::exists(m_finalPath)) {
            std::filesystem::remove(m_finalPath, ec);
        }
        std::filesystem::rename(m_tempPath, m_finalPath, ec);
        if (ec) {
            throw std::runtime_error("Failed to commit atomic write to " + m_finalPath);
        }
    }

    void abort() override {
        std::error_code ec;
        std::filesystem::remove(m_tempPath, ec);
    }

private:
    std::string m_finalPath;
    std::string m_tempPath;
    FileByteWriter m_writer;
};

class DirectFileWriteSession final : public AtomicWriteSession {
public:
    explicit DirectFileWriteSession(std::string path)
        : m_path(std::move(path)), m_writer(m_path) {
    }

    ByteWriter& writer() override {
        return m_writer;
    }

    void commit() override {
        m_writer.flush();
    }

    void abort() override {
    }

private:
    std::string m_path;
    FileByteWriter m_writer;
};

} // namespace

std::unique_ptr<ByteReader> FilesystemBackend::openRead(const std::string& path) {
    return std::make_unique<FileByteReader>(path);
}

std::unique_ptr<AtomicWriteSession> FilesystemBackend::openWrite(const std::string& path, AtomicWriteOptions options) {
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path());

    if (!options.atomic) {
        return std::make_unique<DirectFileWriteSession>(path);
    }

    std::string tempPath = path + ".tmp";
    return std::make_unique<AtomicFileWriteSession>(path, tempPath);
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
