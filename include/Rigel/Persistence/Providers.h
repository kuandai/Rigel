#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Rigel {
namespace Voxel {
class BlockRegistry;
struct BlockID;
}
}

namespace Rigel::Persistence {

class Provider {
public:
    virtual ~Provider() = default;
};

class ProviderRegistry {
public:
    void add(const std::string& id, std::shared_ptr<Provider> provider) {
        m_providers[id] = std::move(provider);
    }

    std::shared_ptr<Provider> find(const std::string& id) const {
        auto it = m_providers.find(id);
        if (it == m_providers.end()) {
            return nullptr;
        }
        return it->second;
    }

    template <typename T>
    std::shared_ptr<T> findAs(const std::string& id) const {
        return std::dynamic_pointer_cast<T>(find(id));
    }

private:
    std::unordered_map<std::string, std::shared_ptr<Provider>> m_providers;
};

inline constexpr const char* kBlockRegistryProviderId = "rigel:persistence.block_registry";

class BlockIdentityProvider : public Provider {
public:
    virtual std::optional<Voxel::BlockID> resolveRuntimeId(std::string_view externalId) const = 0;
    virtual std::optional<std::string> resolveExternalId(Voxel::BlockID runtimeId) const = 0;
    virtual std::optional<std::string> resolveAlias(std::string_view externalId) const = 0;
    virtual Voxel::BlockID placeholderRuntimeId() const = 0;
};

class BlockRegistryProvider final : public BlockIdentityProvider {
public:
    explicit BlockRegistryProvider(const Voxel::BlockRegistry* registry)
        : m_registry(registry) {
    }

    std::optional<Voxel::BlockID> resolveRuntimeId(std::string_view externalId) const override;
    std::optional<std::string> resolveExternalId(Voxel::BlockID runtimeId) const override;
    std::optional<std::string> resolveAlias(std::string_view externalId) const override;
    Voxel::BlockID placeholderRuntimeId() const override;

    void addAlias(std::string externalId, std::string canonicalId);
    void setPlaceholderIdentifier(std::string identifier);

    const Voxel::BlockRegistry* registry() const { return m_registry; }

private:
    const Voxel::BlockRegistry* m_registry = nullptr;
    std::string m_placeholderIdentifier = "base:air";
    std::unordered_map<std::string, std::string> m_aliasToCanonical;
};

} // namespace Rigel::Persistence
