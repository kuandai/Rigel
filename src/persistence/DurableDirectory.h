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

    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        if (component.empty() || component == "." || component == "..") {
            continue;
        }
        createDirectory(current);
        synchronizeDirectory(containingDirectory(current));
    }
}

} // namespace Rigel::Persistence::detail
