#include "TestFramework.h"

#include "Rigel/Voxel/WorldConfigProvider.h"

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
    overlay_enabled: true
)";

    ConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(yaml));
    WorldRenderConfig config = provider.loadRenderConfig();

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
    CHECK(config.profiling.enabled);
    CHECK(config.profiling.overlayEnabled);
}
