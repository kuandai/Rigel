#include "TestFramework.h"

#include "Rigel/Voxel/BlockType.h"

using namespace Rigel::Voxel;

TEST_CASE(FaceTextures_Uniform) {
    FaceTextures textures = FaceTextures::uniform("stone");
    for (size_t i = 0; i < DirectionCount; ++i) {
        CHECK_EQ(textures.faces[i], "stone");
    }
}

TEST_CASE(FaceTextures_TopBottomSides) {
    FaceTextures textures = FaceTextures::topBottomSides("top", "bottom", "side");
    CHECK_EQ(textures.forFace(Direction::PosY), "top");
    CHECK_EQ(textures.forFace(Direction::NegY), "bottom");
    CHECK_EQ(textures.forFace(Direction::PosX), "side");
    CHECK_EQ(textures.forFace(Direction::NegX), "side");
    CHECK_EQ(textures.forFace(Direction::PosZ), "side");
    CHECK_EQ(textures.forFace(Direction::NegZ), "side");
}
