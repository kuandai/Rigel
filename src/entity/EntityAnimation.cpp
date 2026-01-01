#include "Rigel/Entity/EntityAnimation.h"

#include <algorithm>
#include <cmath>

namespace Rigel::Entity {

namespace {
float clampTime(float time, bool loop, float duration) {
    if (duration <= 0.0f) {
        return 0.0f;
    }
    if (loop) {
        float t = std::fmod(time, duration);
        if (t < 0.0f) {
            t += duration;
        }
        return t;
    }
    return std::clamp(time, 0.0f, duration);
}
} // namespace

glm::vec3 EntityAnimationTrack::sample(float time,
                                       bool loop,
                                       float duration,
                                       const glm::vec3& defaultValue) const {
    if (keys.empty()) {
        return defaultValue;
    }
    if (keys.size() == 1) {
        return keys.front().value;
    }

    float t = clampTime(time, loop, duration);
    if (t <= keys.front().time) {
        return keys.front().value;
    }
    if (t >= keys.back().time) {
        return keys.back().value;
    }

    auto upper = std::upper_bound(
        keys.begin(), keys.end(), t,
        [](float value, const EntityKeyframe& frame) { return value < frame.time; }
    );
    if (upper == keys.begin()) {
        return keys.front().value;
    }

    auto lower = upper - 1;
    float span = upper->time - lower->time;
    if (span <= 0.0f) {
        return upper->value;
    }
    float alpha = (t - lower->time) / span;
    return lower->value + (upper->value - lower->value) * alpha;
}

const EntityBoneAnimation* EntityAnimation::findBone(std::string_view name) const {
    auto it = bones.find(std::string(name));
    if (it == bones.end()) {
        return nullptr;
    }
    return &it->second;
}

const EntityAnimation* EntityAnimationSet::find(std::string_view name) const {
    auto it = animations.find(std::string(name));
    if (it == animations.end()) {
        return nullptr;
    }
    return &it->second;
}

} // namespace Rigel::Entity
