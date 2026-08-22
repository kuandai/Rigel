#include "MemoryByteReader.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Rigel::Persistence::Backends::CR {

MemoryByteReader::MemoryByteReader(std::vector<uint8_t> data)
    : m_data(std::move(data)) {
}

uint8_t MemoryByteReader::readU8() {
    ensureAvailable(1);
    return m_data[m_pos++];
}

uint16_t MemoryByteReader::readU16() {
    uint16_t value = 0;
    value |= static_cast<uint16_t>(readU8()) << 8;
    value |= static_cast<uint16_t>(readU8());
    return value;
}

uint32_t MemoryByteReader::readU32() {
    uint32_t value = 0;
    value |= static_cast<uint32_t>(readU8()) << 24;
    value |= static_cast<uint32_t>(readU8()) << 16;
    value |= static_cast<uint32_t>(readU8()) << 8;
    value |= static_cast<uint32_t>(readU8());
    return value;
}

int32_t MemoryByteReader::readI32() {
    return static_cast<int32_t>(readU32());
}

void MemoryByteReader::readBytes(uint8_t* dst, size_t len) {
    ensureAvailable(len);
    if (len == 0) {
        return;
    }
    std::copy_n(m_data.data() + m_pos, len, dst);
    m_pos += len;
}

size_t MemoryByteReader::size() const {
    return m_data.size();
}

size_t MemoryByteReader::tell() const {
    return m_pos;
}

void MemoryByteReader::seek(size_t offset) {
    if (offset > m_data.size()) {
        throw std::runtime_error("CRMemoryReader seek out of range");
    }
    m_pos = offset;
}

std::vector<uint8_t> MemoryByteReader::readAt(size_t offset, size_t len) {
    if (offset > m_data.size() || len > m_data.size() - offset) {
        throw std::runtime_error("CRMemoryReader readAt out of range");
    }
    return std::vector<uint8_t>(m_data.begin() + offset, m_data.begin() + offset + len);
}

void MemoryByteReader::ensureAvailable(size_t len) {
    if (m_pos + len > m_data.size()) {
        throw std::runtime_error("CRMemoryReader read out of range");
    }
}

} // namespace Rigel::Persistence::Backends::CR
