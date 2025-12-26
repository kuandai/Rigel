#include "TestFramework.h"

#include "Rigel/Voxel/TextureAtlas.h"

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
