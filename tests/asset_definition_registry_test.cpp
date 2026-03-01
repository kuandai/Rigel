#include "Rigel/Asset/DefinitionRegistry.h"
#include "TestFramework.h"

#include <string>

using namespace Rigel;

namespace {

Asset::IR::AssetGraphIR makeGraph(bool reverseOrder) {
    Asset::IR::AssetGraphIR graph;

    Asset::IR::EntityDefIR eA;
    eA.identifier = "base:entity_a";
    eA.sourcePath = "entities/a.json";
    eA.modelRef = "models/entities/model_a.json";
    eA.animationRef = "animations/entities/a.animation.json";
    eA.renderMode = "unlit";
    eA.extensions["x"] = "1";

    Asset::IR::EntityDefIR eB;
    eB.identifier = "base:entity_b";
    eB.sourcePath = "entities/b.json";
    eB.modelRef = "models/entities/model_b.json";
    eB.animationRef = "animations/entities/b.animation.json";
    eB.renderMode = "lit";

    Asset::IR::ItemDefIR iA;
    iA.identifier = "base:item_a";
    iA.sourcePath = "items/a.json";
    iA.textureRef = "textures/items/a.png";
    iA.modelRef = "base:item3D";
    iA.renderMode = "base:item3D";

    Asset::IR::ItemDefIR iB;
    iB.identifier = "base:item_b";
    iB.sourcePath = "items/b.json";
    iB.textureRef = "textures/items/b.png";
    iB.modelRef = "base:item2D";
    iB.renderMode = "base:item2D";

    if (reverseOrder) {
        graph.entities.push_back(std::move(eB));
        graph.entities.push_back(std::move(eA));
        graph.items.push_back(std::move(iB));
        graph.items.push_back(std::move(iA));
    } else {
        graph.entities.push_back(std::move(eA));
        graph.entities.push_back(std::move(eB));
        graph.items.push_back(std::move(iA));
        graph.items.push_back(std::move(iB));
    }
    return graph;
}

} // namespace

TEST_CASE(AssetDefinitionRegistry_BuildsDeterministicallyFromGraph) {
    Asset::EntityTypeRegistry entitiesA;
    Asset::ItemDefinitionRegistry itemsA;
    Asset::buildDefinitionRegistriesFromAssetGraph(makeGraph(false), entitiesA, itemsA);

    Asset::EntityTypeRegistry entitiesB;
    Asset::ItemDefinitionRegistry itemsB;
    Asset::buildDefinitionRegistriesFromAssetGraph(makeGraph(true), entitiesB, itemsB);

    CHECK_EQ(entitiesA.size(), 2u);
    CHECK_EQ(itemsA.size(), 2u);
    CHECK_EQ(entitiesA.snapshotHash(), entitiesB.snapshotHash());
    CHECK_EQ(itemsA.snapshotHash(), itemsB.snapshotHash());

    const auto* entity = entitiesA.find("base:entity_a");
    CHECK(entity != nullptr);
    CHECK_EQ(entity->modelRef, "models/entities/model_a.json");
    CHECK_EQ(entity->animationRef, "animations/entities/a.animation.json");

    const auto* item = itemsA.find("base:item_b");
    CHECK(item != nullptr);
    CHECK_EQ(item->textureRef, "textures/items/b.png");
}

TEST_CASE(AssetDefinitionRegistry_ReportsDuplicateIdentifiers) {
    Asset::IR::AssetGraphIR graph;
    Asset::IR::EntityDefIR e1;
    e1.identifier = "base:entity_a";
    e1.sourcePath = "entities/first.json";
    Asset::IR::EntityDefIR e2 = e1;
    e2.sourcePath = "entities/second.json";
    graph.entities.push_back(std::move(e1));
    graph.entities.push_back(std::move(e2));

    Asset::IR::ItemDefIR i1;
    i1.identifier = "base:item_a";
    i1.sourcePath = "items/first.json";
    Asset::IR::ItemDefIR i2 = i1;
    i2.sourcePath = "items/second.json";
    graph.items.push_back(std::move(i1));
    graph.items.push_back(std::move(i2));

    Asset::EntityTypeRegistry entities;
    Asset::ItemDefinitionRegistry items;
    std::vector<Asset::IR::ValidationIssue> diagnostics;
    Asset::buildDefinitionRegistriesFromAssetGraph(graph, entities, items, &diagnostics);

    CHECK_EQ(entities.size(), 1u);
    CHECK_EQ(items.size(), 1u);

    bool sawEntityDup = false;
    bool sawItemDup = false;
    for (const auto& issue : diagnostics) {
        if (issue.field == "entity.identifier" &&
            issue.message.find("Duplicate entity identifier") != std::string::npos) {
            sawEntityDup = true;
        }
        if (issue.field == "item.identifier" &&
            issue.message.find("Duplicate item identifier") != std::string::npos) {
            sawItemDup = true;
        }
    }
    CHECK(sawEntityDup);
    CHECK(sawItemDup);
}
