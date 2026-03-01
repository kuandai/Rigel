#pragma once

#include "Rigel/Asset/AssetIR.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Rigel::Asset {

class EntityTypeRegistry {
public:
    using Definition = IR::EntityDefIR;

    void clear();
    bool registerDefinition(Definition def);
    const Definition* find(std::string_view identifier) const;
    const std::vector<Definition>& definitions() const { return m_definitions; }
    size_t size() const { return m_definitions.size(); }
    uint64_t snapshotHash() const;

private:
    std::vector<Definition> m_definitions;
};

class ItemDefinitionRegistry {
public:
    using Definition = IR::ItemDefIR;

    void clear();
    bool registerDefinition(Definition def);
    const Definition* find(std::string_view identifier) const;
    const std::vector<Definition>& definitions() const { return m_definitions; }
    size_t size() const { return m_definitions.size(); }
    uint64_t snapshotHash() const;

private:
    std::vector<Definition> m_definitions;
};

void buildDefinitionRegistriesFromAssetGraph(
    const IR::AssetGraphIR& graph,
    EntityTypeRegistry& entities,
    ItemDefinitionRegistry& items,
    std::vector<IR::ValidationIssue>* diagnostics = nullptr);

} // namespace Rigel::Asset

