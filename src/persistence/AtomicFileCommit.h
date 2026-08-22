#pragma once

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace Rigel::Persistence::detail {

inline std::filesystem::path containingDirectory(
    const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
}

template <typename Finalize,
          typename SynchronizeFile,
          typename Replace,
          typename SynchronizeDirectory>
void commitAtomicFile(const std::filesystem::path& tempPath,
                      const std::filesystem::path& finalPath,
                      Finalize&& finalize,
                      SynchronizeFile&& synchronizeFile,
                      Replace&& replace,
                      SynchronizeDirectory&& synchronizeDirectory) {
    bool published = false;
    try {
        std::forward<Finalize>(finalize)();
        std::forward<SynchronizeFile>(synchronizeFile)(tempPath);

        std::error_code replaceError;
        std::forward<Replace>(replace)(tempPath, finalPath, replaceError);
        if (replaceError) {
            throw std::runtime_error(
                "Failed to commit atomic write to " + finalPath.string() + ": " + replaceError.message());
        }
        published = true;
        std::forward<SynchronizeDirectory>(synchronizeDirectory)(
            containingDirectory(finalPath));
    } catch (...) {
        if (!published) {
            std::error_code cleanupError;
            std::filesystem::remove(tempPath, cleanupError);
        }
        throw;
    }
}

template <typename Remove, typename SynchronizeDirectory>
void removeFileDurably(const std::filesystem::path& path,
                       Remove&& remove,
                       SynchronizeDirectory&& synchronizeDirectory) {
    if (std::forward<Remove>(remove)(path)) {
        std::forward<SynchronizeDirectory>(synchronizeDirectory)(
            containingDirectory(path));
    }
}

} // namespace Rigel::Persistence::detail
