#include "TestFramework.h"

#include "ResourceRegistry.h"
#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Entity/EntityModelLoader.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

using namespace Rigel::Asset;
using namespace Rigel::Entity;

namespace {

std::string modelWithHitbox(std::string_view min, std::string_view max) {
    return "id: authored\nhitbox:\n  min: [" + std::string(min) +
        "]\n  max: [" + std::string(max) + "]\n";
}

Handle<EntityModelAsset> loadModel(std::string definition) {
    ResourceRegistry::SetEntityModel(std::move(definition));
    AssetManager assets;
    assets.loadManifest("manifest.yaml");
    assets.registerLoader(
        "entity_models", std::make_unique<EntityModelLoader>());
    return assets.get<EntityModelAsset>("entity_models/authored");
}

std::string rejectedModelDiagnostic(std::string definition) {
    try {
        static_cast<void>(loadModel(std::move(definition)));
    } catch (const AssetLoadError& error) {
        CHECK_EQ(error.assetId(), std::string("entity_models/authored"));
        return error.what();
    }
    throw Rigel::Test::TestFailure("Expected entity model load rejection");
}

std::string rejectedDiagnostic(std::string_view min, std::string_view max) {
    return rejectedModelDiagnostic(modelWithHitbox(min, max));
}

void checkBoundRejection(std::string_view min,
                         std::string_view max,
                         std::string_view axis) {
    const std::string diagnostic = rejectedDiagnostic(min, max);
    CHECK(diagnostic.find("hitbox") != std::string::npos);
    CHECK(diagnostic.find(
              "min." + std::string(axis) + " < max." + std::string(axis)) !=
          std::string::npos);
    CHECK(diagnostic.find("models/entities/authored.yaml") !=
          std::string::npos);
}

} // namespace

TEST_CASE(EntityModelLoader_AcceptsValidHitbox) {
    const auto model = loadModel(modelWithHitbox(
        "-0.25, -1.0, -0.75", "0.25, 2.0, 0.75"));

    CHECK(model);
    CHECK(model->hitbox.has_value());
    CHECK_EQ(model->hitbox->min, glm::vec3(-0.25f, -1.0f, -0.75f));
    CHECK_EQ(model->hitbox->max, glm::vec3(0.25f, 2.0f, 0.75f));
}

TEST_CASE(EntityModelLoader_RejectsNonFiniteHitboxCoordinates) {
    const std::array cases = {
        std::array<std::string_view, 3>{"nan, 0, 0", "1, 1, 1", "min.x"},
        std::array<std::string_view, 3>{"0, 0, 0", "1, inf, 1", "max.y"},
    };

    for (const auto& testCase : cases) {
        const std::string diagnostic =
            rejectedDiagnostic(testCase[0], testCase[1]);
        CHECK(diagnostic.find(testCase[2]) != std::string::npos);
        CHECK(diagnostic.find("must be finite") != std::string::npos);
        CHECK(diagnostic.find("models/entities/authored.yaml") !=
              std::string::npos);
    }
}

TEST_CASE(EntityModelLoader_RejectsMalformedHitboxCoordinates) {
    const std::array definitions = {
        std::string("id: authored\nhitbox:\n  min: [0, 0, 0]\n"),
        modelWithHitbox("0, 0, 0, 0", "1, 1, 1"),
    };

    for (const std::string& definition : definitions) {
        const std::string diagnostic = rejectedModelDiagnostic(definition);
        CHECK(diagnostic.find("three-coordinate 'min' and 'max' sequences") !=
              std::string::npos);
        CHECK(diagnostic.find("models/entities/authored.yaml") !=
              std::string::npos);
    }
}

TEST_CASE(EntityModelLoader_RejectsInvertedHitboxBoundsOnEveryAxis) {
    checkBoundRejection("2, 0, 0", "1, 1, 1", "x");
    checkBoundRejection("0, 2, 0", "1, 1, 1", "y");
    checkBoundRejection("0, 0, 2", "1, 1, 1", "z");
}

TEST_CASE(EntityModelLoader_RejectsZeroHitboxExtentOnEveryAxis) {
    checkBoundRejection("1, 0, 0", "1, 1, 1", "x");
    checkBoundRejection("0, 1, 0", "1, 1, 1", "y");
    checkBoundRejection("0, 0, 1", "1, 1, 1", "z");
}
