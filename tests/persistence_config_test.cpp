#include "TestFramework.h"

#include "Rigel/Persistence/PersistenceConfig.h"
#include "Rigel/Persistence/PersistenceConfigProvider.h"

#include <optional>

using namespace Rigel::Persistence;

namespace {

class StringConfigSource : public Rigel::Config::IConfigSource {
public:
    explicit StringConfigSource(std::string yaml, std::string sourceName = "string")
        : m_yaml(std::move(yaml))
        , m_sourceName(std::move(sourceName))
    {}

    std::optional<std::string> load() const override { return m_yaml; }
    std::string name() const override { return m_sourceName; }

private:
    std::string m_yaml;
    std::string m_sourceName;
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
)";

    config.applyYaml("test", yaml);

    CHECK_EQ(config.format, "cr");
    CHECK(config.crLz4Enabled);
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
    CHECK(!config.crLz4Enabled);
    config.applyYaml("overlay", overlay);

    CHECK(config.crLz4Enabled);
    config.applyYaml("format-only", "persistence:\n  format: memory\n");
    CHECK_EQ(config.format, "memory");
    CHECK(config.crLz4Enabled);
}

TEST_CASE(PersistenceConfigProvider_LoadsSourcesInOrder) {
    PersistenceConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(R"(
persistence:
  format: cr
  providers:
    rigel:persistence.cr:
      lz4: false
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
    CHECK(config.crLz4Enabled);
}

TEST_CASE(PersistenceConfig_MissingLz4UsesTypedDefault) {
    PersistenceConfig config;
    config.applyYaml("missing.yaml", "persistence:\n  format: memory\n");

    CHECK_EQ(config.format, "memory");
    CHECK(!config.crLz4Enabled);
}

TEST_CASE(PersistenceConfig_InvalidLz4IsDiagnosticAndAtomic) {
    for (const std::string& invalid : {
             std::string("TRUE"),
             std::string("False"),
             std::string("yes"),
             std::string("no"),
             std::string("1"),
             std::string("0"),
             std::string("maybe"),
             std::string("[]"),
             std::string("{}"),
             std::string("~")}) {
        PersistenceConfig config;
        config.applyYaml(
            "base.yaml",
            "persistence:\n"
            "  format: memory\n"
            "  providers:\n"
            "    rigel:persistence.cr:\n"
            "      lz4: true\n");

        std::string diagnostic;
        try {
            config.applyYaml(
                "invalid-provider.yaml",
                "persistence:\n"
                "  format: cr\n"
                "  providers:\n"
                "    rigel:persistence.cr:\n"
                "      lz4: " + invalid + "\n");
        } catch (const std::invalid_argument& error) {
            diagnostic = error.what();
        }

        CHECK(
            diagnostic.find(
                "persistence.providers.rigel:persistence.cr.lz4") !=
            std::string::npos);
        CHECK(
            diagnostic.find("invalid-provider.yaml") != std::string::npos);
        CHECK(
            diagnostic.find("expected boolean 'true' or 'false'") !=
            std::string::npos);
        CHECK_EQ(config.format, "memory");
        CHECK(config.crLz4Enabled);
    }
}
