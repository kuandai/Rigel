#pragma once

#include <memory>
#include <string>

namespace Rigel::Asset {

/// Type-safe, reference-counted handle to a loaded asset.
/// Handles are lightweight and can be freely copied.
template<typename T>
class Handle {
public:
    Handle() = default;

    Handle(std::shared_ptr<T> asset, std::string id)
        : m_asset(std::move(asset))
        , m_id(std::move(id))
    {}

    /// Access the underlying asset
    T* operator->() const { return m_asset.get(); }
    T& operator*() const { return *m_asset; }

    /// Check if handle is valid
    explicit operator bool() const { return m_asset != nullptr; }

    /// Get the asset identifier
    const std::string& id() const { return m_id; }

    /// Get raw pointer (use sparingly)
    T* get() const { return m_asset.get(); }

    /// Get shared ownership pointer
    std::shared_ptr<T> shared() const { return m_asset; }

    /// Comparison operators
    bool operator==(const Handle& other) const { return m_asset == other.m_asset; }
    bool operator!=(const Handle& other) const { return m_asset != other.m_asset; }

private:
    std::shared_ptr<T> m_asset;
    std::string m_id;
};

} // namespace Rigel::Asset
