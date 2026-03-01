#include "Rigel/Asset/DefinitionRegistry.h"

#include <algorithm>
#include <array>

namespace Rigel::Asset {

namespace {

constexpr uint64_t kFNV64Offset = 14695981039346656037ull;
constexpr uint64_t kFNV64Prime = 1099511628211ull;

void hashBytes(uint64_t& hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFNV64Prime;
    }
}

template <typename T>
void hashPod(uint64_t& hash, const T& value) {
    hashBytes(hash, &value, sizeof(T));
}

void hashString(uint64_t& hash, const std::string& value) {
    hashBytes(hash, value.data(), value.size());
    static constexpr uint8_t separator = 0xff;
    hashBytes(hash, &separator, sizeof(separator));
}

void hashOptionalVec3(uint64_t& hash, const std::optional<std::array<float, 3>>& value) {
    const uint8_t present = value.has_value() ? 1u : 0u;
    hashPod(hash, present);
    if (!value.has_value()) {
        return;
    }
    for (float component : *value) {
        hashPod(hash, component);
    }
}

template <typename DefT>
uint64_t hashDefinitionVector(const std::vector<DefT>& defs, const std::string& schemaTag) {
    uint64_t hash = kFNV64Offset;
    hashString(hash, schemaTag);
    const uint64_t count = static_cast<uint64_t>(defs.size());
    hashPod(hash, count);
    for (const auto& def : defs) {
        hashString(hash, def.identifier);
        hashString(hash, def.sourcePath);
        hashString(hash, def.modelRef);
        hashString(hash, def.animationRef);
        hashString(hash, def.renderMode);
        hashOptionalVec3(hash, def.renderOffset);
        hashOptionalVec3(hash, def.hitboxMin);
        hashOptionalVec3(hash, def.hitboxMax);
        std::vector<std::pair<std::string, std::string>> ext(def.extensions.begin(),
                                                             def.extensions.end());
        std::sort(ext.begin(), ext.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const uint64_t extCount = static_cast<uint64_t>(ext.size());
        hashPod(hash, extCount);
        for (const auto& [key, value] : ext) {
            hashString(hash, key);
            hashString(hash, value);
        }
    }
    return hash;
}

template <>
uint64_t hashDefinitionVector<ItemDefinitionRegistry::Definition>(
    const std::vector<ItemDefinitionRegistry::Definition>& defs,
    const std::string& schemaTag) {
    uint64_t hash = kFNV64Offset;
    hashString(hash, schemaTag);
    const uint64_t count = static_cast<uint64_t>(defs.size());
    hashPod(hash, count);
    for (const auto& def : defs) {
        hashString(hash, def.identifier);
        hashString(hash, def.sourcePath);
        hashString(hash, def.modelRef);
        hashString(hash, def.textureRef);
        hashString(hash, def.renderMode);
        std::vector<std::pair<std::string, std::string>> ext(def.extensions.begin(),
                                                             def.extensions.end());
        std::sort(ext.begin(), ext.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const uint64_t extCount = static_cast<uint64_t>(ext.size());
        hashPod(hash, extCount);
        for (const auto& [key, value] : ext) {
            hashString(hash, key);
            hashString(hash, value);
        }
    }
    return hash;
}

template <typename DefT>
void sortDefinitions(std::vector<DefT>& defs) {
    std::sort(defs.begin(),
              defs.end(),
              [](const DefT& a, const DefT& b) {
                  if (a.identifier == b.identifier) {
                      return a.sourcePath < b.sourcePath;
                  }
                  return a.identifier < b.identifier;
              });
}

template <typename DefT>
bool hasDuplicateIdentifier(const std::vector<DefT>& defs, std::string_view id) {
    size_t count = 0;
    for (const auto& def : defs) {
        if (def.identifier == id) {
            ++count;
            if (count > 1) {
                return true;
            }
        }
    }
    return false;
}

template <typename DefT>
void pushDefinition(IR::ValidationSeverity severity,
                    std::vector<IR::ValidationIssue>* diagnostics,
                    const DefT& def,
                    const char* field,
                    const std::string& message) {
    if (!diagnostics) {
        return;
    }
    diagnostics->push_back(IR::ValidationIssue{
        .severity = severity,
        .sourcePath = def.sourcePath,
        .identifier = def.identifier,
        .field = field,
        .message = message
    });
}

} // namespace

void EntityTypeRegistry::clear() {
    m_definitions.clear();
}

bool EntityTypeRegistry::registerDefinition(Definition def) {
    if (def.identifier.empty()) {
        return false;
    }
    if (find(def.identifier) != nullptr) {
        return false;
    }
    m_definitions.push_back(std::move(def));
    return true;
}

const EntityTypeRegistry::Definition* EntityTypeRegistry::find(std::string_view identifier) const {
    auto it = std::find_if(m_definitions.begin(), m_definitions.end(),
                           [&](const Definition& def) { return def.identifier == identifier; });
    if (it == m_definitions.end()) {
        return nullptr;
    }
    return &(*it);
}

uint64_t EntityTypeRegistry::snapshotHash() const {
    return hashDefinitionVector(m_definitions, "Rigel.EntityTypeRegistry.v1");
}

void ItemDefinitionRegistry::clear() {
    m_definitions.clear();
}

bool ItemDefinitionRegistry::registerDefinition(Definition def) {
    if (def.identifier.empty()) {
        return false;
    }
    if (find(def.identifier) != nullptr) {
        return false;
    }
    m_definitions.push_back(std::move(def));
    return true;
}

const ItemDefinitionRegistry::Definition* ItemDefinitionRegistry::find(std::string_view identifier) const {
    auto it = std::find_if(m_definitions.begin(), m_definitions.end(),
                           [&](const Definition& def) { return def.identifier == identifier; });
    if (it == m_definitions.end()) {
        return nullptr;
    }
    return &(*it);
}

uint64_t ItemDefinitionRegistry::snapshotHash() const {
    return hashDefinitionVector(m_definitions, "Rigel.ItemDefinitionRegistry.v1");
}

void buildDefinitionRegistriesFromAssetGraph(
    const IR::AssetGraphIR& graph,
    EntityTypeRegistry& entities,
    ItemDefinitionRegistry& items,
    std::vector<IR::ValidationIssue>* diagnostics) {
    entities.clear();
    items.clear();

    std::vector<IR::EntityDefIR> entityDefs = graph.entities;
    std::vector<IR::ItemDefIR> itemDefs = graph.items;
    sortDefinitions(entityDefs);
    sortDefinitions(itemDefs);

    for (const auto& def : entityDefs) {
        if (def.identifier.empty()) {
            pushDefinition(IR::ValidationSeverity::Error,
                           diagnostics,
                           def,
                           "entity.identifier",
                           "Entity definition identifier is empty");
            continue;
        }
        if (hasDuplicateIdentifier(entityDefs, def.identifier)) {
            pushDefinition(IR::ValidationSeverity::Warning,
                           diagnostics,
                           def,
                           "entity.identifier",
                           "Duplicate entity identifier in compiled graph");
        }
        entities.registerDefinition(def);
    }

    for (const auto& def : itemDefs) {
        if (def.identifier.empty()) {
            pushDefinition(IR::ValidationSeverity::Error,
                           diagnostics,
                           def,
                           "item.identifier",
                           "Item definition identifier is empty");
            continue;
        }
        if (hasDuplicateIdentifier(itemDefs, def.identifier)) {
            pushDefinition(IR::ValidationSeverity::Warning,
                           diagnostics,
                           def,
                           "item.identifier",
                           "Duplicate item identifier in compiled graph");
        }
        items.registerDefinition(def);
    }
}

} // namespace Rigel::Asset
