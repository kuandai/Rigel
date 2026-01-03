#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Rigel::Persistence {

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

struct AtomicWriteOptions {
    bool atomic = true;
    bool replaceExisting = true;
};

class AtomicWriteSession {
public:
    virtual ~AtomicWriteSession() = default;

    virtual ByteWriter& writer() = 0;
    virtual void commit() = 0;
    virtual void abort() = 0;
};

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual std::unique_ptr<ByteReader> openRead(const std::string& path) = 0;
    virtual std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path, AtomicWriteOptions options) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual std::vector<std::string> list(const std::string& path) = 0;
    virtual void mkdirs(const std::string& path) = 0;
    virtual void remove(const std::string& path) = 0;
};

class FilesystemBackend : public StorageBackend {
public:
    std::unique_ptr<ByteReader> openRead(const std::string& path) override;
    std::unique_ptr<AtomicWriteSession> openWrite(const std::string& path, AtomicWriteOptions options) override;
    bool exists(const std::string& path) override;
    std::vector<std::string> list(const std::string& path) override;
    void mkdirs(const std::string& path) override;
    void remove(const std::string& path) override;
};

} // namespace Rigel::Persistence
