#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class ResourceRegistry {
public:
    static std::span<const char> Get(const std::string& path) {
        static constexpr char duplicateField[] =
            "namespace: test\n"
            "assets:\n"
            "  generator_definitions:\n"
            "    default:\n"
            "      path: generators/first.yaml\n"
            "      path: generators/second.yaml\n";
        static constexpr char duplicateName[] =
            "namespace: test\n"
            "assets:\n"
            "  generator_definitions:\n"
            "    default:\n"
            "      path: generators/first.yaml\n"
            "    default:\n"
            "      path: generators/second.yaml\n";

        if (path == "duplicate_generator_field.yaml") {
            return {duplicateField, sizeof(duplicateField) - 1};
        }
        if (path == "duplicate_generator_name.yaml") {
            return {duplicateName, sizeof(duplicateName) - 1};
        }
        throw std::runtime_error("Resource not found: " + path);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> paths;
        return paths;
    }
};
