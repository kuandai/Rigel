#pragma once

#include "Rigel/Persistence/Storage.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Rigel::Persistence::Backends::CR {

class MemoryByteReader final : public ByteReader {
public:
    explicit MemoryByteReader(std::vector<uint8_t> data);

    uint8_t readU8() override;
    uint16_t readU16() override;
    uint32_t readU32() override;
    int32_t readI32() override;
    void readBytes(uint8_t* dst, size_t len) override;

    size_t size() const override;
    size_t tell() const override;
    void seek(size_t offset) override;
    std::vector<uint8_t> readAt(size_t offset, size_t len) override;

private:
    void ensureAvailable(size_t len);

    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

} // namespace Rigel::Persistence::Backends::CR
