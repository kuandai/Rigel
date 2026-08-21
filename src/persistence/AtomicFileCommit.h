#pragma once

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace Rigel::Persistence::detail {

template <typename Finalize, typename Replace>
void commitAtomicFile(const std::filesystem::path& tempPath,
                      const std::filesystem::path& finalPath,
                      Finalize&& finalize,
                      Replace&& replace) {
    try {
        std::forward<Finalize>(finalize)();

        std::error_code replaceError;
        std::forward<Replace>(replace)(tempPath, finalPath, replaceError);
        if (replaceError) {
            throw std::runtime_error(
                "Failed to commit atomic write to " + finalPath.string() + ": " + replaceError.message());
        }
    } catch (...) {
        std::error_code cleanupError;
        std::filesystem::remove(tempPath, cleanupError);
        throw;
    }
}

} // namespace Rigel::Persistence::detail
