#include "TestFramework.h"

#include "Rigel/Persistence/Providers.h"
#include "Rigel/Voxel/BlockRegistry.h"
#include "Rigel/Voxel/BlockType.h"

using namespace Rigel;

namespace {

Voxel::BlockID registerOpaqueBlock(Voxel::BlockRegistry& registry, const std::string& identifier) {
    Voxel::BlockType block;
    block.identifier = identifier;
    block.isOpaque = true;
    block.isSolid = true;
    return registry.registerBlock(identifier, std::move(block));
}

} // namespace

TEST_CASE(Persistence_BlockRegistryProvider_AliasAndPlaceholder) {
    Voxel::BlockRegistry registry;
    Voxel::BlockID stoneId = registerOpaqueBlock(registry, "base:stone");
    Voxel::BlockID placeholderId = registerOpaqueBlock(registry, "base:placeholder");

    Persistence::BlockRegistryProvider provider(&registry);
    provider.addAlias("legacy:stone", "base:stone");
    provider.setPlaceholderIdentifier("base:placeholder");

    auto runtimeAlias = provider.resolveRuntimeId("legacy:stone");
    CHECK(runtimeAlias.has_value());
    if (runtimeAlias) {
        CHECK_EQ(*runtimeAlias, stoneId);
    }

    auto runtimeLegacyNamespace = provider.resolveRuntimeId("rigel:stone");
    CHECK(runtimeLegacyNamespace.has_value());
    if (runtimeLegacyNamespace) {
        CHECK_EQ(*runtimeLegacyNamespace, stoneId);
    }

    auto external = provider.resolveExternalId(stoneId);
    CHECK(external.has_value());
    if (external) {
        CHECK_EQ(*external, "base:stone");
    }

    CHECK_EQ(provider.placeholderRuntimeId(), placeholderId);
}
