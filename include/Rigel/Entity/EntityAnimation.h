#pragma once

#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rigel::Entity {

struct EntityKeyframe {
    float time = 0.0f;
    glm::vec3 value{0.0f};
};

struct EntityAnimationTrack {
    std::vector<EntityKeyframe> keys;

    glm::vec3 sample(float time,
                     bool loop,
                     float duration,
                     const glm::vec3& defaultValue) const;
};

struct EntityBoneAnimation {
    EntityAnimationTrack position;
    EntityAnimationTrack rotation;
    EntityAnimationTrack scale;
};

struct EntityAnimation {
    float duration = 0.0f;
    bool loop = true;
    std::unordered_map<std::string, EntityBoneAnimation> bones;

    const EntityBoneAnimation* findBone(std::string_view name) const;
};

struct EntityAnimationSet {
    std::unordered_map<std::string, EntityAnimation> animations;

    const EntityAnimation* find(std::string_view name) const;
};

} // namespace Rigel::Entity
