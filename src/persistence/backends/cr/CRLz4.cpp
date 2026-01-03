#include "Rigel/Persistence/Backends/CR/CRLz4.h"

#include <dlfcn.h>
#include <stdexcept>

namespace Rigel::Persistence::Backends::CR {

namespace {

using CompressBoundFn = int (*)(int inputSize);
using CompressFn = int (*)(const char* src, char* dst, int srcSize, int dstCapacity);
using DecompressFn = int (*)(const char* src, char* dst, int compressedSize, int dstCapacity);

class Lz4Library {
public:
    Lz4Library() {
        handle = dlopen("liblz4.so.1", RTLD_LAZY);
        if (!handle) {
            handle = dlopen("liblz4.so", RTLD_LAZY);
        }
        if (!handle) {
            return;
        }
        compressBound = reinterpret_cast<CompressBoundFn>(dlsym(handle, "LZ4_compressBound"));
        compress = reinterpret_cast<CompressFn>(dlsym(handle, "LZ4_compress_default"));
        decompress = reinterpret_cast<DecompressFn>(dlsym(handle, "LZ4_decompress_safe"));
        if (!compressBound || !compress || !decompress) {
            dlclose(handle);
            handle = nullptr;
            compressBound = nullptr;
            compress = nullptr;
            decompress = nullptr;
        }
    }

    ~Lz4Library() {
        if (handle) {
            dlclose(handle);
        }
    }

    bool available() const {
        return handle && compressBound && compress && decompress;
    }

    int bound(int size) const {
        return compressBound(size);
    }

    int doCompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) const {
        return compress(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst),
            static_cast<int>(srcSize), static_cast<int>(dstCapacity));
    }

    int doDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) const {
        return decompress(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst),
            static_cast<int>(srcSize), static_cast<int>(dstCapacity));
    }

private:
    void* handle = nullptr;
    CompressBoundFn compressBound = nullptr;
    CompressFn compress = nullptr;
    DecompressFn decompress = nullptr;
};

Lz4Library& instance() {
    static Lz4Library lib;
    return lib;
}

} // namespace

bool CRLz4::available() {
    return instance().available();
}

int CRLz4::compressBound(int inputSize) {
    if (!available()) {
        throw std::runtime_error("CRLz4: library not available");
    }
    return instance().bound(inputSize);
}

int CRLz4::compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
    if (!available()) {
        throw std::runtime_error("CRLz4: library not available");
    }
    return instance().doCompress(src, srcSize, dst, dstCapacity);
}

int CRLz4::decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
    if (!available()) {
        throw std::runtime_error("CRLz4: library not available");
    }
    return instance().doDecompress(src, srcSize, dst, dstCapacity);
}

} // namespace Rigel::Persistence::Backends::CR
