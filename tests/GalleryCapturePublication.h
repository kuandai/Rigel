#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
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

using GalleryCaptureDirectoryExchange = std::function<std::error_code(
    const std::filesystem::path& staging,
    const std::filesystem::path& destination)>;

void publishGalleryCaptureSet(
    const std::vector<NamedGalleryFramebufferCapture>& captures,
    const std::filesystem::path& destination,
    const GalleryCaptureDirectoryExchange& directoryExchange = {});

} // namespace Rigel::Test
