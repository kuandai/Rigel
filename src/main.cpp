#include "Rigel/Application.h"
#include "Rigel/Asset/AssetAudit.h"
#include <cstdlib>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::optional<std::filesystem::path> resolveCrRoot(int argc, char** argv, int startIndex) {
    if (startIndex < argc && argv[startIndex][0] != '-') {
        return std::filesystem::path(argv[startIndex]);
    }
    if (const char* env = std::getenv("RIGEL_CR_ASSET_ROOT")) {
        if (*env != '\0') {
            return std::filesystem::path(env);
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--asset-audit") {
            std::optional<std::filesystem::path> outputPath;
            std::optional<std::filesystem::path> crRoot;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg(argv[i]);
                if (arg == "--output") {
                    if (i + 1 >= argc) {
                        spdlog::error("--output requires a path");
                        return 2;
                    }
                    outputPath = std::filesystem::path(argv[++i]);
                    continue;
                }
                if (!crRoot) {
                    crRoot = resolveCrRoot(argc, argv, i);
                    if (crRoot) {
                        continue;
                    }
                }
                spdlog::error("Unknown argument for --asset-audit: {}", argv[i]);
                return 2;
            }
            if (!crRoot) {
                crRoot = resolveCrRoot(argc, argv, argc);
            }
            if (!crRoot) {
                spdlog::error("Usage: Rigel --asset-audit <cr_root> [--output <report.json>]");
                spdlog::error("Or set RIGEL_CR_ASSET_ROOT and run: Rigel --asset-audit");
                return 2;
            }
            return Rigel::Asset::runAssetAuditTool(*crRoot, outputPath);
        }

        Rigel::Application app;
        app.run();
    } catch (const std::exception& e) {
        spdlog::error("Application error: {}", e.what());
        return -1;
    }
    return 0;
}
