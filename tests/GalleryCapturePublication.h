#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Rigel::Test {

struct GalleryFramebufferCapture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
};

struct NamedGalleryFramebufferCapture {
    std::string name;
    GalleryFramebufferCapture framebuffer;
};

void publishGalleryCaptureSet(
    const std::vector<NamedGalleryFramebufferCapture>& captures,
    const std::filesystem::path& destination);

} // namespace Rigel::Test
