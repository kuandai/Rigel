#include "TestFramework.h"
#include "OpenGLFixture.h"

#include "Rigel/Asset/AssetManager.h"

using namespace Rigel::Asset;

namespace {

struct InputAsset final : AssetBase {};

class InputLoader final : public IAssetLoader {
public:
    std::string_view category() const override { return "input"; }

    std::shared_ptr<AssetBase> load(const LoadContext&) override {
        return std::make_shared<InputAsset>();
    }
};

template <typename T>
class CountingLoader final : public IAssetLoader {
public:
    CountingLoader(std::string_view category, int& loadCount)
        : m_category(category)
        , m_loadCount(loadCount) {}

    std::string_view category() const override { return m_category; }

    std::shared_ptr<AssetBase> load(const LoadContext&) override {
        ++m_loadCount;
        return std::make_shared<T>();
    }

private:
    std::string m_category;
    int& m_loadCount;
};

} // namespace

TEST_CASE(AssetManager_LoadsEmbeddedManifest) {
    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    CHECK(!assets.exists("raw/streaming_config"));
    CHECK(!assets.exists("raw/render_config"));
    CHECK(!assets.exists("raw/persistence_config"));
    CHECK(assets.exists("shaders/voxel"));
    CHECK(!assets.exists("blocks/dirt"));
    CHECK(assets.exists("entity_models/demo_cube"));
    CHECK(assets.exists("entity_anims/demo_spin"));
}

TEST_CASE(AssetManager_ShaderEntriesHaveRequiredStages) {
    AssetManager assets;
    CHECK_NO_THROW(assets.loadManifest("manifest.yaml"));

    size_t shaderCount = 0;
    assets.forEachInCategory("shaders", [&](const std::string& name,
                                                const AssetManager::AssetEntry& entry) {
        const auto vertex = entry.getString("vertex");
        const auto fragment = entry.getString("fragment");
        if (!vertex || vertex->empty()) {
            throw Rigel::Test::TestFailure(
                "Shader entry '" + name + "' is missing its vertex source");
        }
        if (!fragment || fragment->empty()) {
            throw Rigel::Test::TestFailure(
                "Shader entry '" + name + "' is missing its fragment source");
        }
        ++shaderCount;
    });
    CHECK(shaderCount > 0);
}

TEST_CASE(AssetManager_CustomLoaderBeforeManifestKeepsGraphicsLoaders) {
    Rigel::Test::HiddenOpenGLContext context;
    context.require();

    AssetManager assets;
    assets.registerLoader("input", std::make_unique<InputLoader>());
    assets.loadManifest("manifest.yaml");

    const auto texture = assets.get<TextureAsset>("textures/entity_debug");
    CHECK_NE(texture->id, 0u);
    CHECK(texture->width > 0);
    CHECK(texture->height > 0);

    const auto shader = assets.get<ShaderAsset>("shaders/voxel");
    CHECK_NE(shader->program, 0u);
}

TEST_CASE(AssetManager_ManifestLoadingPreservesBuiltinReplacements) {
    int textureLoads = 0;
    int shaderLoads = 0;

    AssetManager assets;
    assets.registerLoader(
        "textures",
        std::make_unique<CountingLoader<TextureAsset>>("textures", textureLoads));
    assets.registerLoader(
        "shaders",
        std::make_unique<CountingLoader<ShaderAsset>>("shaders", shaderLoads));
    assets.loadManifest("manifest.yaml");

    CHECK(assets.get<TextureAsset>("textures/entity_debug"));
    CHECK(assets.get<ShaderAsset>("shaders/voxel"));
    CHECK_EQ(textureLoads, 1);
    CHECK_EQ(shaderLoads, 1);
}
