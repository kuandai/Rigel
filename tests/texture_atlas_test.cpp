#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Voxel/TextureAtlas.h"

#include <array>
#include <limits>
#include <string>
#include <vector>

using namespace Rigel::Voxel;

TEST_CASE(TextureAtlas_AddAndLookup) {
    TextureAtlas atlas;
    std::vector<unsigned char> pixels(atlas.tileSize() * atlas.tileSize() * 4, 255);

    TextureHandle handle = atlas.addTexture("tex", pixels.data());
    CHECK(handle.isValid());
    CHECK_EQ(atlas.textureCount(), static_cast<size_t>(1));
    CHECK_EQ(atlas.findTexture("tex").index, handle.index);

    TextureCoords coords = atlas.getUVs(handle);
    float halfPixel = 0.5f / static_cast<float>(atlas.tileSize());
    CHECK_NEAR(coords.u0, halfPixel, 0.0001f);
    CHECK_NEAR(coords.v0, halfPixel, 0.0001f);
    CHECK_NEAR(coords.u1, 1.0f - halfPixel, 0.0001f);
    CHECK_NEAR(coords.v1, 1.0f - halfPixel, 0.0001f);
}

TEST_CASE(TextureAtlas_PreservesLayersAcrossByteBoundary) {
    TextureAtlas atlas;
    std::vector<unsigned char> pixels(
        static_cast<size_t>(atlas.tileSize() * atlas.tileSize() * 4), 255);

    TextureHandle belowBoundary;
    TextureHandle aboveBoundary;
    for (size_t layer = 0; layer <= 256; ++layer) {
        const TextureHandle handle = atlas.addTexture(
            "textures/test/layer_" + std::to_string(layer), pixels.data());
        if (layer == 255) {
            belowBoundary = handle;
        } else if (layer == 256) {
            aboveBoundary = handle;
        }
    }

    CHECK_EQ(belowBoundary.index, static_cast<uint16_t>(255));
    CHECK_EQ(aboveBoundary.index, static_cast<uint16_t>(256));
    CHECK_EQ(atlas.getLayer(belowBoundary), 255);
    CHECK_EQ(atlas.getLayer(aboveBoundary), 256);
    CHECK_EQ(atlas.getUVs(belowBoundary).layer, 255);
    CHECK_EQ(atlas.getUVs(aboveBoundary).layer, 256);
}

TEST_CASE(TextureAtlas_UploadsAndBindsLayersAcrossByteBoundary) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    GLint hardwareMaxLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &hardwareMaxLayers);
    if (hardwareMaxLayers < 257) {
        SKIP_TEST(
            "OpenGL context exposes fewer than 257 array texture layers");
    }

    TextureAtlas atlas({
        .tileSize = 1,
        .maxLayers = std::numeric_limits<uint16_t>::max(),
        .generateMipmaps = false,
    });
    constexpr std::array<unsigned char, 4> pixels = {255, 255, 255, 255};
    for (size_t layer = 0; layer <= 256; ++layer) {
        atlas.addTexture(
            "textures/test/gl_layer_" + std::to_string(layer), pixels.data());
    }

    atlas.upload();

    atlas.bind(3);
    GLint uploadedLayers = 0;
    glGetTexLevelParameteriv(
        GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &uploadedLayers);
    CHECK_EQ(uploadedLayers, 257);

    atlas.bindTint(4);
    GLint uploadedTintLayers = 0;
    glGetTexLevelParameteriv(
        GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &uploadedTintLayers);
    CHECK_EQ(uploadedTintLayers, 257);
    CHECK_EQ(glGetError(), GL_NO_ERROR);
}

TEST_CASE(TextureAtlas_RejectsSetsBeyondOpenGLArrayLayerLimit) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    GLint hardwareMaxLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &hardwareMaxLayers);
    if (hardwareMaxLayers <= 0 ||
        hardwareMaxLayers >= std::numeric_limits<uint16_t>::max()) {
        SKIP_TEST(
            "OpenGL array texture limit cannot be exceeded by a valid atlas");
    }

    TextureAtlas atlas({
        .tileSize = 1,
        .maxLayers = std::numeric_limits<uint16_t>::max(),
        .generateMipmaps = false,
    });
    constexpr std::array<unsigned char, 4> pixels = {255, 255, 255, 255};
    const size_t requestedLayers = static_cast<size_t>(hardwareMaxLayers) + 1;
    for (size_t layer = 0; layer < requestedLayers; ++layer) {
        atlas.addTexture(
            "textures/test/excess_layer_" + std::to_string(layer), pixels.data());
    }

    try {
        atlas.upload();
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find(std::to_string(requestedLayers)) != std::string::npos);
        CHECK(message.find(std::to_string(hardwareMaxLayers)) != std::string::npos);
        CHECK(message.find("GL_MAX_ARRAY_TEXTURE_LAYERS") != std::string::npos);
        CHECK(message.find("Reduce the generated block texture set") !=
              std::string::npos);
        return;
    }

    throw Rigel::Test::TestFailure(
        "TextureAtlas upload accepted more layers than OpenGL supports");
}
