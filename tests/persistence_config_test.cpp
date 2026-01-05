#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceConfig.h"

using namespace Rigel::Persistence;

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
    }
}
