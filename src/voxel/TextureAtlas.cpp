#include "Rigel/Voxel/TextureAtlas.h"
#include "ResourceRegistry.h"

#include <stb_image.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <cstring>

namespace Rigel::Voxel {

namespace {
std::array<unsigned char, 4> computeAverageTint(const unsigned char* pixels, size_t pixelCount) {
    if (!pixels || pixelCount == 0) {
        return {255, 255, 255, 255};
    }

    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t sumA = 0;
    for (size_t i = 0; i < pixelCount; ++i) {
        size_t base = i * 4;
        sumR += pixels[base + 0];
        sumG += pixels[base + 1];
        sumB += pixels[base + 2];
        sumA += pixels[base + 3];
    }

    uint64_t count = pixelCount;
    unsigned char r = static_cast<unsigned char>(sumR / count);
    unsigned char g = static_cast<unsigned char>(sumG / count);
    unsigned char b = static_cast<unsigned char>(sumB / count);
    unsigned char a = static_cast<unsigned char>(sumA / count);
    return {r, g, b, a};
}
} // namespace

TextureAtlas::TextureAtlas()
    : m_config{}
{
}

TextureAtlas::TextureAtlas(const Config& config)
    : m_config(config)
{
}

TextureAtlas::~TextureAtlas() {
    releaseGPU();
}

TextureAtlas::TextureAtlas(TextureAtlas&& other) noexcept
    : m_config(other.m_config)
    , m_textureArray(other.m_textureArray)
    , m_tintArray(other.m_tintArray)
    , m_entries(std::move(other.m_entries))
    , m_pathToHandle(std::move(other.m_pathToHandle))
{
    other.m_textureArray = 0;
    other.m_tintArray = 0;
}

TextureAtlas& TextureAtlas::operator=(TextureAtlas&& other) noexcept {
    if (this != &other) {
        releaseGPU();
        m_config = other.m_config;
        m_textureArray = other.m_textureArray;
        m_tintArray = other.m_tintArray;
        m_entries = std::move(other.m_entries);
        m_pathToHandle = std::move(other.m_pathToHandle);
        other.m_textureArray = 0;
        other.m_tintArray = 0;
    }
    return *this;
}

TextureHandle TextureAtlas::addTexture(const std::string& path, const unsigned char* pixels) {
    // Check if already added
    auto it = m_pathToHandle.find(path);
    if (it != m_pathToHandle.end()) {
        return it->second;
    }

    // Check layer limit
    if (m_entries.size() >= static_cast<size_t>(m_config.maxLayers)) {
        throw std::runtime_error("TextureAtlas: maximum layer count exceeded");
    }

    // Create entry
    TextureEntry entry;
    entry.path = path;
    entry.layer = static_cast<int>(m_entries.size());

    // Copy pixel data
    size_t pixelDataSize = static_cast<size_t>(m_config.tileSize * m_config.tileSize * 4);
    entry.pixels.resize(pixelDataSize);
    std::memcpy(entry.pixels.data(), pixels, pixelDataSize);
    entry.tint = computeAverageTint(entry.pixels.data(),
                                    static_cast<size_t>(m_config.tileSize * m_config.tileSize));

    TextureHandle handle{static_cast<uint16_t>(m_entries.size())};
    m_entries.push_back(std::move(entry));
    m_pathToHandle[path] = handle;

    spdlog::debug("TextureAtlas: added texture {} at layer {}", path, handle.index);

    return handle;
}

TextureHandle TextureAtlas::addTextureFromResource(const std::string& path) {
    // Check if already added
    auto it = m_pathToHandle.find(path);
    if (it != m_pathToHandle.end()) {
        return it->second;
    }

    // Load from resource registry
    auto data = ResourceRegistry::Get(path);
    if (data.empty()) {
        throw std::runtime_error("TextureAtlas: resource not found: " + path);
    }

    // Decode with stb_image
    int width, height, channels;
    stbi_set_flip_vertically_on_load(true);

    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()),
        &width, &height, &channels, 4  // Force RGBA
    );

    if (!pixels) {
        throw std::runtime_error(
            "TextureAtlas: failed to load " + path + ": " + stbi_failure_reason()
        );
    }

    // Verify size
    if (width != m_config.tileSize || height != m_config.tileSize) {
        stbi_image_free(pixels);
        throw std::runtime_error(
            "TextureAtlas: texture " + path + " has wrong size " +
            std::to_string(width) + "x" + std::to_string(height) +
            " (expected " + std::to_string(m_config.tileSize) + "x" +
            std::to_string(m_config.tileSize) + ")"
        );
    }

    // Add to atlas
    TextureHandle handle = addTexture(path, pixels);
    stbi_image_free(pixels);

    return handle;
}

TextureHandle TextureAtlas::findTexture(const std::string& path) const {
    auto it = m_pathToHandle.find(path);
    if (it == m_pathToHandle.end()) {
        return TextureHandle::invalid();
    }
    return it->second;
}

TextureCoords TextureAtlas::getUVs(TextureHandle handle) const {
    if (handle.index >= m_entries.size()) {
        return {0, 0, 1, 1, 0};
    }

    // Full tile UVs with half-pixel inset to prevent bleeding
    float halfPixel = 0.5f / static_cast<float>(m_config.tileSize);

    return {
        halfPixel,          // u0
        halfPixel,          // v0
        1.0f - halfPixel,   // u1
        1.0f - halfPixel,   // v1
        m_entries[handle.index].layer
    };
}

int TextureAtlas::getLayer(TextureHandle handle) const {
    if (handle.index >= m_entries.size()) {
        return 0;
    }
    return m_entries[handle.index].layer;
}

void TextureAtlas::upload() {
    if (m_entries.empty()) {
        spdlog::warn("TextureAtlas: no textures to upload");
        return;
    }

    // Create texture if needed
    if (m_textureArray == 0) {
        glGenTextures(1, &m_textureArray);
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, m_textureArray);

    // Allocate storage for all layers
    GLsizei layerCount = static_cast<GLsizei>(m_entries.size());
    GLsizei tileSize = static_cast<GLsizei>(m_config.tileSize);

    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,                  // mipmap level
        GL_RGBA8,           // internal format
        tileSize,           // width
        tileSize,           // height
        layerCount,         // depth (layer count)
        0,                  // border
        GL_RGBA,            // format
        GL_UNSIGNED_BYTE,   // type
        nullptr             // no initial data
    );

    // Upload each layer
    for (size_t i = 0; i < m_entries.size(); i++) {
        const TextureEntry& entry = m_entries[i];

        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY,
            0,                              // mipmap level
            0, 0, static_cast<GLint>(i),    // x, y, layer offset
            tileSize, tileSize, 1,          // width, height, depth
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            entry.pixels.data()
        );
    }

    // Set filtering
    if (m_config.generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Set wrapping
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    if (m_tintArray == 0) {
        glGenTextures(1, &m_tintArray);
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, m_tintArray);

    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        GL_RGBA8,
        1,
        1,
        layerCount,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr
    );

    for (size_t i = 0; i < m_entries.size(); i++) {
        const TextureEntry& entry = m_entries[i];
        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY,
            0,
            0, 0, static_cast<GLint>(i),
            1, 1, 1,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            entry.tint.data()
        );
    }

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    spdlog::info("TextureAtlas: uploaded {} textures ({}x{} each)",
                 m_entries.size(), m_config.tileSize, m_config.tileSize);
}

void TextureAtlas::bind(GLuint unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_textureArray);
}

void TextureAtlas::bindTint(GLuint unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_tintArray);
}

void TextureAtlas::releaseGPU() {
    if (m_textureArray != 0) {
        glDeleteTextures(1, &m_textureArray);
        m_textureArray = 0;
    }
    if (m_tintArray != 0) {
        glDeleteTextures(1, &m_tintArray);
        m_tintArray = 0;
    }
}

} // namespace Rigel::Voxel
