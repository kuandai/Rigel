#pragma once

#include <memory>
#include <string>
#include <unordered_map>

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

} // namespace Rigel::Persistence
