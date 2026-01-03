#pragma once

#include <cstddef>
#include <cstdint>

namespace Rigel::Persistence::Backends::CR {

class CRLz4 {
public:
    static bool available();
    static int compressBound(int inputSize);
    static int compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity);
    static int decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity);
};

} // namespace Rigel::Persistence::Backends::CR
