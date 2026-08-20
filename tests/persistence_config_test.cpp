#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceConfig.h"
#include "Rigel/Persistence/PersistenceConfigProvider.h"

#include <optional>

using namespace Rigel::Persistence;

namespace {

class StringConfigSource : public Rigel::Config::IConfigSource {
public:
    explicit StringConfigSource(std::string yaml)
        : m_yaml(std::move(yaml))
    {}

    std::optional<std::string> load() const override { return m_yaml; }
    std::string name() const override { return "string"; }

private:
    std::string m_yaml;
};

} // namespace

TEST_CASE(PersistenceConfig_ApplyYaml) {
    PersistenceConfig config;
    std::string yaml = R"(
persistence:
  format: cr
  providers:
    rigel:persistence.cr:
      lz4: true
    rigel:persistence.other:
      mode: debug
)";

    config.applyYaml("test", yaml);

    CHECK_EQ(config.format, "cr");
    const ProviderConfig* cr = config.findProvider("rigel:persistence.cr");
    CHECK(cr != nullptr);
    if (cr) {
        CHECK(cr->getBool("lz4", false));
    }
    const ProviderConfig* other = config.findProvider("rigel:persistence.other");
    CHECK(other != nullptr);
    if (other) {
        CHECK_EQ(other->getString("mode", ""), "debug");
        CHECK(!other->getBool("missing", false));
    }
}

TEST_CASE(PersistenceConfig_OverlayMergesProviders) {
    PersistenceConfig config;
    std::string base = R"(
persistence:
  format: cr
  providers:
    rigel:persistence.cr:
      lz4: false
      mode: safe
)";
    std::string overlay = R"(
persistence:
  providers:
    rigel:persistence.cr:
      lz4: true
)";

    config.applyYaml("base", base);
    config.applyYaml("overlay", overlay);

    const ProviderConfig* cr = config.findProvider("rigel:persistence.cr");
    CHECK(cr != nullptr);
    if (cr) {
        CHECK(cr->getBool("lz4", false));
        CHECK_EQ(cr->getString("mode", ""), "safe");
    }
}

TEST_CASE(PersistenceConfigProvider_LoadsSourcesInOrder) {
    PersistenceConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
persistence:
  format: cr
  providers:
    rigel:persistence.cr:
      lz4: false
      mode: safe
)"));
    provider.addSource(std::make_unique<StringConfigSource>(R"(
persistence:
  format: memory
  providers:
    rigel:persistence.cr:
      lz4: true
)"));

    const PersistenceConfig config = provider.load();

    CHECK_EQ(config.format, "memory");
    const ProviderConfig* cr = config.findProvider("rigel:persistence.cr");
    CHECK(cr != nullptr);
    if (cr) {
        CHECK(cr->getBool("lz4", false));
        CHECK_EQ(cr->getString("mode", ""), "safe");
    }
}
