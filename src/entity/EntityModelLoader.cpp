#include "Rigel/Entity/EntityModelLoader.h"

#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Asset/AssetManager.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

namespace Rigel::Entity {

namespace {
std::optional<std::string> getString(ryml::ConstNodeRef node, const char* key) {
    if (!node.readable() || !node.has_child(ryml::to_csubstr(key))) {
        return std::nullopt;
    }
    std::string value;
    node[ryml::to_csubstr(key)] >> value;
    return value;
}

bool readVec2(ryml::ConstNodeRef node, glm::vec2& out) {
    if (!node.readable() || !node.is_seq() || node.num_children() < 2) {
        return false;
    }
    node[0] >> out.x;
    node[1] >> out.y;
    return true;
}

bool readVec3(ryml::ConstNodeRef node, glm::vec3& out) {
    if (!node.readable() || !node.is_seq() || node.num_children() < 3) {
        return false;
    }
    node[0] >> out.x;
    node[1] >> out.y;
    node[2] >> out.z;
    return true;
}

void sortTrack(EntityAnimationTrack& track) {
    std::sort(track.keys.begin(), track.keys.end(),
              [](const EntityKeyframe& a, const EntityKeyframe& b) {
                  return a.time < b.time;
              });
}

EntityAnimationTrack parseTrack(ryml::ConstNodeRef node) {
    EntityAnimationTrack track;
    if (!node.readable() || !node.is_seq()) {
        return track;
    }
    for (ryml::ConstNodeRef keyframe : node.children()) {
        if (!keyframe.is_map()) {
            continue;
        }
        EntityKeyframe frame;
        if (keyframe.has_child("time")) {
            keyframe["time"] >> frame.time;
        }
        if (keyframe.has_child("value")) {
            readVec3(keyframe["value"], frame.value);
        }
        track.keys.push_back(frame);
    }
    sortTrack(track);
    return track;
}
} // namespace

std::shared_ptr<Asset::AssetBase> EntityModelLoader::load(const Asset::LoadContext& ctx) {
    auto pathOpt = getString(ctx.config, "path");
    if (!pathOpt) {
        throw Asset::AssetLoadError(ctx.id, "Entity model missing 'path' field");
    }

    auto data = ctx.loadResource(*pathOpt);
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(pathOpt->c_str()),
        ryml::csubstr(data.data(), data.size())
    );
    ryml::ConstNodeRef root = tree.rootref();

    auto asset = std::make_shared<EntityModelAsset>();

    if (root.has_child("texture_width")) {
        root["texture_width"] >> asset->texWidth;
    }
    if (root.has_child("texture_height")) {
        root["texture_height"] >> asset->texHeight;
    }
    if (root.has_child("model_scale")) {
        root["model_scale"] >> asset->modelScale;
    }
    if (root.has_child("default_animation")) {
        root["default_animation"] >> asset->defaultAnimation;
    }
    if (root.has_child("animation_set")) {
        std::string animId;
        root["animation_set"] >> animId;
        if (!animId.empty() && ctx.manager.exists(animId)) {
            asset->animationSet = ctx.manager.get<EntityAnimationSetAsset>(animId);
        }
    }

    if (root.has_child("textures")) {
        ryml::ConstNodeRef textures = root["textures"];
        for (ryml::ConstNodeRef entry : textures.children()) {
            std::string key(entry.key().data(), entry.key().size());
            std::string value;
            entry >> value;
            if (!key.empty() && !value.empty()) {
                asset->textures.emplace(std::move(key), std::move(value));
            }
        }
    }

    std::unordered_map<std::string, std::string> parentNames;

    if (root.has_child("bones")) {
        ryml::ConstNodeRef bones = root["bones"];
        for (ryml::ConstNodeRef boneNode : bones.children()) {
            if (!boneNode.is_map()) {
                continue;
            }
            EntityBone bone;
            if (boneNode.has_child("name")) {
                boneNode["name"] >> bone.name;
            }
            if (boneNode.has_child("pivot")) {
                readVec3(boneNode["pivot"], bone.pivot);
            }
            if (boneNode.has_child("rotation")) {
                readVec3(boneNode["rotation"], bone.rotation);
            }
            if (boneNode.has_child("scale")) {
                readVec3(boneNode["scale"], bone.scale);
            }

            if (boneNode.has_child("parent")) {
                std::string parentName;
                boneNode["parent"] >> parentName;
                if (!parentName.empty()) {
                    parentNames[bone.name] = parentName;
                }
            }

            if (boneNode.has_child("cubes")) {
                ryml::ConstNodeRef cubes = boneNode["cubes"];
                for (ryml::ConstNodeRef cubeNode : cubes.children()) {
                    if (!cubeNode.is_map()) {
                        continue;
                    }
                    EntityModelCube cube;
                    if (cubeNode.has_child("origin")) {
                        readVec3(cubeNode["origin"], cube.origin);
                    }
                    if (cubeNode.has_child("size")) {
                        readVec3(cubeNode["size"], cube.size);
                    }
                    if (cubeNode.has_child("uv")) {
                        readVec2(cubeNode["uv"], cube.uv);
                    }
                    if (cubeNode.has_child("pivot")) {
                        readVec3(cubeNode["pivot"], cube.pivot);
                    }
                    if (cubeNode.has_child("rotation")) {
                        readVec3(cubeNode["rotation"], cube.rotation);
                    }
                    if (cubeNode.has_child("inflate")) {
                        cubeNode["inflate"] >> cube.inflate;
                    }
                    if (cubeNode.has_child("mirror")) {
                        cubeNode["mirror"] >> cube.mirror;
                    }
                    bone.cubes.push_back(cube);
                }
            }

            asset->bones.push_back(std::move(bone));
        }
    }

    asset->boneLookup.reserve(asset->bones.size());
    for (size_t i = 0; i < asset->bones.size(); ++i) {
        const std::string& name = asset->bones[i].name;
        if (!name.empty()) {
            asset->boneLookup[name] = i;
        }
    }

    for (auto& bone : asset->bones) {
        auto parentIt = parentNames.find(bone.name);
        if (parentIt == parentNames.end()) {
            continue;
        }
        auto lookupIt = asset->boneLookup.find(parentIt->second);
        if (lookupIt == asset->boneLookup.end()) {
            spdlog::warn("EntityModelAsset: parent bone '{}' not found", parentIt->second);
            continue;
        }
        bone.parentIndex = static_cast<int>(lookupIt->second);
    }

    return asset;
}

std::shared_ptr<Asset::AssetBase> EntityAnimationSetLoader::load(const Asset::LoadContext& ctx) {
    auto pathOpt = getString(ctx.config, "path");
    if (!pathOpt) {
        throw Asset::AssetLoadError(ctx.id, "Entity animation set missing 'path' field");
    }

    auto data = ctx.loadResource(*pathOpt);
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(pathOpt->c_str()),
        ryml::csubstr(data.data(), data.size())
    );
    ryml::ConstNodeRef root = tree.rootref();

    auto asset = std::make_shared<EntityAnimationSetAsset>();

    if (!root.has_child("animations")) {
        return asset;
    }

    ryml::ConstNodeRef anims = root["animations"];
    for (ryml::ConstNodeRef animNode : anims.children()) {
        if (!animNode.is_map()) {
            continue;
        }
        EntityAnimation animation;
        std::string animName(animNode.key().data(), animNode.key().size());
        if (animNode.has_child("duration")) {
            animNode["duration"] >> animation.duration;
        }
        if (animNode.has_child("loop")) {
            animNode["loop"] >> animation.loop;
        }
        if (animNode.has_child("bones")) {
            ryml::ConstNodeRef bonesNode = animNode["bones"];
            for (ryml::ConstNodeRef boneNode : bonesNode.children()) {
                if (!boneNode.is_map()) {
                    continue;
                }
                std::string boneName(boneNode.key().data(), boneNode.key().size());
                EntityBoneAnimation boneAnim;
                if (boneNode.has_child("position")) {
                    boneAnim.position = parseTrack(boneNode["position"]);
                }
                if (boneNode.has_child("rotation")) {
                    boneAnim.rotation = parseTrack(boneNode["rotation"]);
                }
                if (boneNode.has_child("scale")) {
                    boneAnim.scale = parseTrack(boneNode["scale"]);
                }
                animation.bones.emplace(std::move(boneName), std::move(boneAnim));
            }
        }
        asset->set.animations.emplace(std::move(animName), std::move(animation));
    }

    return asset;
}

} // namespace Rigel::Entity
