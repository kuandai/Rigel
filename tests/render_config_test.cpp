#include "TestFramework.h"

#include "Rigel/Render/RenderConfigProvider.h"

#include <functional>
#include <stdexcept>

using namespace Rigel::Config;
using namespace Rigel::Render;
using namespace Rigel::Voxel;

namespace {

class StringConfigSource : public IConfigSource {
public:
    explicit StringConfigSource(std::string yaml)
        : m_yaml(std::move(yaml))
    {}

    std::optional<std::string> load() const override {
        return m_yaml;
    }

    std::string name() const override {
        return "string";
    }

private:
    std::string m_yaml;
};

std::string exceptionMessage(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    throw Rigel::Test::TestFailure("Expected invalid configuration");
}

} // namespace

TEST_CASE(RenderConfig_ApplyYaml) {
    const std::string yaml = R"(
render:
  sun_direction: [0.2, 0.8, 0.1]
  transparent_alpha: 0.4
  render_distance: 300.0
  shadow:
    enabled: true
    cascades: 2
    map_size: 512
    max_distance: 150.0
    split_lambda: 0.6
    bias: 0.001
    normal_bias: 0.01
    pcf_radius: 2
    pcf_radius_near: 1
    pcf_radius_far: 3
    transparent_scale: 0.75
    strength: 1.8
    fade_power: 1.5
  taa:
    enabled: true
    blend: 0.8
    jitter_scale: 1.5
  profiling:
    enabled: true
)";

    RenderConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(yaml));
    WorldRenderConfig config = provider.load();

    CHECK_NEAR(config.sunDirection.x, 0.2f, 0.0001f);
    CHECK_NEAR(config.sunDirection.y, 0.8f, 0.0001f);
    CHECK_NEAR(config.sunDirection.z, 0.1f, 0.0001f);
    CHECK_NEAR(config.transparentAlpha, 0.4f, 0.0001f);
    CHECK_NEAR(config.renderDistance, 300.0f, 0.0001f);
    CHECK(config.shadow.enabled);
    CHECK_EQ(config.shadow.cascades, 2);
    CHECK_EQ(config.shadow.mapSize, 512);
    CHECK_NEAR(config.shadow.maxDistance, 150.0f, 0.0001f);
    CHECK_NEAR(config.shadow.splitLambda, 0.6f, 0.0001f);
    CHECK_NEAR(config.shadow.bias, 0.001f, 0.0001f);
    CHECK_NEAR(config.shadow.normalBias, 0.01f, 0.0001f);
    CHECK_EQ(config.shadow.pcfRadius, 2);
    CHECK_EQ(config.shadow.pcfRadiusNear, 1);
    CHECK_EQ(config.shadow.pcfRadiusFar, 3);
    CHECK_NEAR(config.shadow.transparentScale, 0.75f, 0.0001f);
    CHECK_NEAR(config.shadow.strength, 1.8f, 0.0001f);
    CHECK_NEAR(config.shadow.fadePower, 1.5f, 0.0001f);
    CHECK(config.taa.enabled);
    CHECK_NEAR(config.taa.blend, 0.8f, 0.0001f);
    CHECK_NEAR(config.taa.jitterScale, 1.5f, 0.0001f);
    CHECK(config.profilingEnabled);
}

TEST_CASE(RenderConfig_LayeredShadowRetainsOmittedValues) {
    RenderConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    enabled: true
    pcf_radius: 2
    pcf_radius_near: 1
    pcf_radius_far: 3
)"));
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    enabled: false
)"));

    const WorldRenderConfig config = provider.load();

    CHECK(!config.shadow.enabled);
    CHECK_EQ(config.shadow.pcfRadius, 2);
    CHECK_EQ(config.shadow.pcfRadiusNear, 1);
    CHECK_EQ(config.shadow.pcfRadiusFar, 3);
}

TEST_CASE(RenderConfig_GenericPcfRadiusSuppliesNearAndFarFallbacks) {
    RenderConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    pcf_radius: 4
)"));

    const WorldRenderConfig config = provider.load();

    CHECK_EQ(config.shadow.pcfRadius, 4);
    CHECK_EQ(config.shadow.pcfRadiusNear, 4);
    CHECK_EQ(config.shadow.pcfRadiusFar, 4);
}

TEST_CASE(RenderConfig_LayeredGenericPcfRadiusPreservesSpecificOverrides) {
    RenderConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    pcf_radius: 2
    pcf_radius_near: 1
    pcf_radius_far: 3
)"));
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    pcf_radius: 4
)"));

    const WorldRenderConfig config = provider.load();

    CHECK_EQ(config.shadow.pcfRadius, 4);
    CHECK_EQ(config.shadow.pcfRadiusNear, 1);
    CHECK_EQ(config.shadow.pcfRadiusFar, 3);
}

TEST_CASE(RenderConfig_LayeredGenericPcfRadiusUpdatesUnspecifiedFallback) {
    RenderConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    pcf_radius: 2
    pcf_radius_near: 1
)"));
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    pcf_radius: 4
)"));

    const WorldRenderConfig config = provider.load();

    CHECK_EQ(config.shadow.pcfRadius, 4);
    CHECK_EQ(config.shadow.pcfRadiusNear, 1);
    CHECK_EQ(config.shadow.pcfRadiusFar, 4);
}

TEST_CASE(RenderConfig_AcceptsShadowResourceMaxima) {
    RenderConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
render:
  shadow:
    cascades: 4
    map_size: 8192
    pcf_radius: 4
    pcf_radius_near: 4
    pcf_radius_far: 4
)"));

    const WorldRenderConfig config = provider.load();

    CHECK_EQ(config.shadow.cascades, ShadowConfig::MaxCascades);
    CHECK_EQ(config.shadow.mapSize, ShadowConfig::MaxMapSize);
    CHECK_EQ(config.shadow.pcfRadius, ShadowConfig::MaxPcfRadius);
    CHECK_EQ(config.shadow.pcfRadiusNear, ShadowConfig::MaxPcfRadius);
    CHECK_EQ(config.shadow.pcfRadiusFar, ShadowConfig::MaxPcfRadius);
}

TEST_CASE(RenderConfig_RejectsShadowValuesAboveResourceMaxima) {
    const std::string cascadeError = exceptionMessage([] {
        RenderConfigProvider provider;
        provider.addSource(std::make_unique<StringConfigSource>(
            "render:\n  shadow:\n    cascades: 5\n"
        ));
        provider.load();
    });
    CHECK_EQ(
        cascadeError,
        "Invalid configuration value 'render.shadow.cascades' in 'string': "
        "expected integer no greater than 4, got '5'"
    );

    const std::string mapError = exceptionMessage([] {
        RenderConfigProvider provider;
        provider.addSource(std::make_unique<StringConfigSource>(
            "render:\n  shadow:\n    map_size: 8193\n"
        ));
        provider.load();
    });
    CHECK_EQ(
        mapError,
        "Invalid configuration value 'render.shadow.map_size' in 'string': "
        "expected integer no greater than 8192, got '8193'"
    );

    const std::string pcfMaxError = exceptionMessage([] {
        RenderConfigProvider provider;
        provider.addSource(std::make_unique<StringConfigSource>(
            "render:\n  shadow:\n    pcf_radius_near: 5\n"
        ));
        provider.load();
    });
    CHECK_EQ(
        pcfMaxError,
        "Invalid configuration value 'render.shadow.pcf_radius_near' in "
        "'string': expected integer no greater than 4, got '5'"
    );

    const std::string pcfError = exceptionMessage([] {
        RenderConfigProvider provider;
        provider.addSource(std::make_unique<StringConfigSource>(
            "render:\n  shadow:\n    pcf_radius_far: 2147483647\n"
        ));
        provider.load();
    });
    CHECK_EQ(
        pcfError,
        "Invalid configuration value 'render.shadow.pcf_radius_far' in "
        "'string': expected integer no greater than 4, got '2147483647'"
    );

    const std::string unsignedError = exceptionMessage([] {
        RenderConfigProvider provider;
        provider.addSource(std::make_unique<StringConfigSource>(
            "render:\n  shadow:\n    map_size: 4294967295\n"
        ));
        provider.load();
    });
    CHECK_EQ(
        unsignedError,
        "Invalid configuration value 'render.shadow.map_size' in 'string': "
        "expected integer no greater than 8192, got '4294967295'"
    );
}
