#pragma once

#include "AtomicFileCommit.h"

#include <filesystem>

namespace Rigel::Persistence::detail {

template <typename CreateDirectory, typename SynchronizeDirectory>
void createDirectoriesDurably(
    const std::filesystem::path& path,
    CreateDirectory&& createDirectory,
    SynchronizeDirectory&& synchronizeDirectory) {
    if (path.empty()) {
        return;
    }

    const auto normalized = path.lexically_normal();
    std::filesystem::path current = normalized.root_path();
    for (const auto& component : normalized.relative_path()) {
        if (component == ".") {
            continue;
        }
        current /= component;
        createDirectory(current);
        synchronizeDirectory(containingDirectory(current));
    }
}

} // namespace Rigel::Persistence::detail
