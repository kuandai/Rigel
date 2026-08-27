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
        static constexpr char aggregateMissingResource[] =
            "namespace: failed-missing\n"
            "assets:\n"
            "  raw:\n"
            "    stable:\n"
            "      path: changed.txt\n"
            "    provisional:\n"
            "      path: provisional.txt\n"
            "  generator_definitions:\n"
            "    a_valid:\n"
            "      path: generators/valid.yaml\n"
            "    z_missing:\n"
            "      path: generators/missing.yaml\n";
        static constexpr char aggregateInvalidPayload[] =
            "namespace: failed-aggregate\n"
            "assets:\n"
            "  raw:\n"
            "    stable:\n"
            "      path: changed.txt\n"
            "    provisional:\n"
            "      path: provisional.txt\n"
            "  generator_definitions:\n"
            "    a_valid:\n"
            "      path: generators/valid.yaml\n"
            "    z_invalid:\n"
            "      path: generators/invalid.yaml\n";
        static constexpr char malformedDeclaration[] =
            "namespace: failed-malformed\n"
            "assets:\n"
            "  raw:\n"
            "    required:\n"
            "      path: required.txt\n"
            "  generator_definitions:\n"
            "    default:\n"
            "      path: [generators/valid.yaml]\n";
        static constexpr char initialManifest[] =
            "namespace: initial\n"
            "assets:\n"
            "  raw:\n"
            "    stable:\n"
            "      path: stable.txt\n"
            "  generator_definitions:\n"
            "    stable:\n"
            "      path: generators/valid.yaml\n";
        static constexpr char correctedManifest[] =
            "namespace: corrected\n"
            "assets:\n"
            "  raw:\n"
            "    stable:\n"
            "      path: corrected.txt\n"
            "    committed:\n"
            "      path: committed.txt\n"
            "  generator_definitions:\n"
            "    corrected:\n"
            "      path: generators/corrected.yaml\n";
        static constexpr char laterOrdinaryManifest[] =
            "namespace: later-ordinary\n"
            "assets:\n"
            "  raw:\n"
            "    stable:\n"
            "      path: later.txt\n";
        static constexpr char malformedManifestYaml[] =
            "namespace: malformed\n"
            "assets:\n"
            "  raw: [\n";
        static constexpr char validDefinition[] = R"yaml(generator:
  schema_version: 2
  id: test:valid
  source_revision: 1
  label: Valid
  description: Aggregate loading fixture.
  bounds:
    min_y: 0
    max_y: 15
  terrain:
    sea_level: 0
    solid_material: test:stone
    water_material: test:water
    density_output: terrain
  climate:
    latitude_scale: 0
    latitude_strength: 0
    local_blend: 0
    global:
      temperature:
        octaves: 1
        frequency: 1
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      humidity:
        octaves: 1
        frequency: 1
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      continentalness:
        octaves: 1
        frequency: 1
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
    local:
      temperature:
        octaves: 1
        frequency: 1
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      humidity:
        octaves: 1
        frequency: 1
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
      continentalness:
        octaves: 1
        frequency: 1
        lacunarity: 2
        persistence: 0.5
        scale: 1
        offset: 0
  biomes:
    blend_power: 2
    epsilon: 0.001
    coast:
      biome: coast
      min_continentalness: -0.1
      max_continentalness: 0.1
    entries:
      - id: land
        target:
          temperature: 0
          humidity: 0
          continentalness: 0
        weight: 1
        water_fill: false
        surface:
          - material: test:surface
            depth: 1
      - id: coast
        target:
          temperature: 0
          humidity: 0
          continentalness: 0
        weight: 1
        water_fill: false
        surface:
          - material: test:surface
            depth: 1
  density_graph:
    outputs:
      terrain: solid
    nodes:
      - id: solid
        type: constant
        value: 1
  caves:
    enabled: false
  structures:
    enabled: false
)yaml";
        static constexpr char stable[] = "retained";
        static constexpr char changed[] = "changed provisionally";
        static constexpr char provisional[] = "new provisional entry";
        static constexpr char required[] = "ordinary required asset";
        static constexpr char corrected[] = "corrected committed entry";
        static constexpr char committed[] = "new committed entry";
        static constexpr char later[] = "later ordinary entry";
        static constexpr char invalidDefinition[] =
            "generator:\n"
            "  schema_version: 2\n"
            "  id: test:invalid\n"
            "  source_revision: 1\n"
            "  label: Invalid\n"
            "  description: Invalid aggregate fixture.\n"
            "  unknown_field: rejected\n";
        static const std::string correctedDefinition = [] {
            std::string result(
                validDefinition, sizeof(validDefinition) - 1);
            result.replace(
                result.find("id: test:valid"),
                std::string("id: test:valid").size(),
                "id: test:corrected");
            result.replace(
                result.find("source_revision: 1"),
                std::string("source_revision: 1").size(),
                "source_revision: 9");
            result.replace(
                result.find("value: 1"),
                std::string("value: 1").size(),
                "value: 0.5");
            return result;
        }();

        if (path == "duplicate_generator_field.yaml") {
            return {duplicateField, sizeof(duplicateField) - 1};
        }
        if (path == "duplicate_generator_name.yaml") {
            return {duplicateName, sizeof(duplicateName) - 1};
        }
        if (path == "aggregate_missing_resource.yaml") {
            return {aggregateMissingResource,
                    sizeof(aggregateMissingResource) - 1};
        }
        if (path == "aggregate_invalid_payload.yaml") {
            return {aggregateInvalidPayload,
                    sizeof(aggregateInvalidPayload) - 1};
        }
        if (path == "malformed_generator_declaration.yaml") {
            return {malformedDeclaration, sizeof(malformedDeclaration) - 1};
        }
        if (path == "initial.yaml") {
            return {initialManifest, sizeof(initialManifest) - 1};
        }
        if (path == "corrected.yaml") {
            return {correctedManifest, sizeof(correctedManifest) - 1};
        }
        if (path == "later_ordinary.yaml") {
            return {laterOrdinaryManifest,
                    sizeof(laterOrdinaryManifest) - 1};
        }
        if (path == "malformed_manifest_yaml.yaml") {
            return {malformedManifestYaml,
                    sizeof(malformedManifestYaml) - 1};
        }
        if (path == "generators/valid.yaml") {
            return {validDefinition, sizeof(validDefinition) - 1};
        }
        if (path == "generators/invalid.yaml") {
            return {invalidDefinition, sizeof(invalidDefinition) - 1};
        }
        if (path == "generators/corrected.yaml") {
            return {correctedDefinition.data(), correctedDefinition.size()};
        }
        if (path == "stable.txt") {
            return {stable, sizeof(stable) - 1};
        }
        if (path == "changed.txt") {
            return {changed, sizeof(changed) - 1};
        }
        if (path == "provisional.txt") {
            return {provisional, sizeof(provisional) - 1};
        }
        if (path == "required.txt") {
            return {required, sizeof(required) - 1};
        }
        if (path == "corrected.txt") {
            return {corrected, sizeof(corrected) - 1};
        }
        if (path == "committed.txt") {
            return {committed, sizeof(committed) - 1};
        }
        if (path == "later.txt") {
            return {later, sizeof(later) - 1};
        }
        throw std::runtime_error("Resource not found: " + path);
    }

    static const std::vector<std::string_view>& Paths() {
        static const std::vector<std::string_view> paths;
        return paths;
    }
};
