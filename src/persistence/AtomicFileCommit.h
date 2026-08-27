#pragma once

#include "Rigel/Persistence/Storage.h"

#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace Rigel::Persistence::detail {

inline std::string atomicCommitFailureMessage(
    const std::filesystem::path& finalPath,
    AtomicFilePublicationState state,
    const std::exception& error) {
    return "Failed to commit atomic write to " + finalPath.string() +
        (state == AtomicFilePublicationState::NotPublished
             ? " before publication: "
             : " after publication; durability is uncertain: ") +
        error.what();
}

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
            throw std::system_error(replaceError);
        }
        published = true;
        std::forward<SynchronizeDirectory>(synchronizeDirectory)(
            containingDirectory(finalPath));
    } catch (const std::exception& error) {
        if (!published) {
            std::error_code cleanupError;
            std::filesystem::remove(tempPath, cleanupError);
        }
        const auto state = published
            ? AtomicFilePublicationState::PublishedDurabilityUncertain
            : AtomicFilePublicationState::NotPublished;
        throw AtomicFilePublicationError(
            state,
            atomicCommitFailureMessage(finalPath, state, error));
    } catch (...) {
        if (!published) {
            std::error_code cleanupError;
            std::filesystem::remove(tempPath, cleanupError);
        }
        const auto state = published
            ? AtomicFilePublicationState::PublishedDurabilityUncertain
            : AtomicFilePublicationState::NotPublished;
        throw AtomicFilePublicationError(
            state,
            "Failed to commit atomic write to " + finalPath.string() +
                (published
                     ? " after publication; durability is uncertain"
                     : " before publication"));
    }
}

template <typename Remove, typename SynchronizeDirectory>
void removeFileDurably(const std::filesystem::path& path,
                       Remove&& remove,
                       SynchronizeDirectory&& synchronizeDirectory) {
    std::forward<Remove>(remove)(path);
    std::forward<SynchronizeDirectory>(synchronizeDirectory)(
        containingDirectory(path));
}

} // namespace Rigel::Persistence::detail
