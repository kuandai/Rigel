#include "TestFramework.h"
#include "Rigel/Entity/Entity.h"

#include <array>
#include <limits>

using namespace Rigel::Entity;

namespace {
void rejectsWithoutMutation(Entity& entity, const EntitySimulationState& invalid) {
    const auto before = entity.simulationState();
    const auto bounds = entity.worldBounds();
    CHECK_THROWS(entity.restoreSimulationState(invalid));
    CHECK_EQ(entity.simulationState(), before);
    CHECK_EQ(entity.worldBounds().min, bounds.min);
    CHECK_EQ(entity.worldBounds().max, bounds.max);
}
}

TEST_CASE(EntitySimulationState_restores_complete_valid_state_and_clears_old_tags) {
    Entity entity;
    entity.addTag("old");
    auto state = entity.simulationState();
    state.id = {1, 2, 3};
    state.position = {3, 4, 5};
    state.velocity = {1, 2, 3};
    state.acceleration = {2, 3, 4};
    state.viewDirection = {0, 1, 0};
    state.gravityModifier = 0.5f;
    state.floorFriction = 0.2f;
    state.onGround = state.collidedX = state.collidedY = state.collidedZ = true;
    state.localBounds = {{-0.25f, -0.5f, -0.75f}, {0.25f, 0.5f, 0.75f}};
    state.tags = {"first", "second"};
    state.modelIdentifier = "rigel:synthetic_model";
    state.renderTint = {0.1f, 0.2f, 0.3f, 0.4f};
    entity.restoreSimulationState(state);
    CHECK_EQ(entity.simulationState(), state);
    CHECK(!entity.hasTag("old"));
    CHECK_EQ(entity.worldBounds().min, state.localBounds.min + state.position);
    CHECK_EQ(entity.worldBounds().max, state.localBounds.max + state.position);

    const auto defaults = Entity{}.simulationState();
    entity.restoreSimulationState(defaults);
    CHECK_EQ(entity.simulationState(), defaults);
}

TEST_CASE(EntitySimulationState_rejects_nonfinite_coordinates_and_rule_state) {
    Entity entity;
    entity.setPosition({3, 4, 5});
    entity.addTag("retained");
    const auto original = entity.simulationState();
    const std::array values{
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    for (const float value : values) {
        for (int field = 0; field < 6; ++field) {
            for (int axis = 0; axis < 3; ++axis) {
                auto state = original;
                const std::array<glm::vec3*, 6> vectors{
                    &state.position, &state.velocity, &state.acceleration,
                    &state.viewDirection, &state.localBounds.min, &state.localBounds.max};
                (*vectors[field])[axis] = value;
                rejectsWithoutMutation(entity, state);
            }
        }
        auto state = original;
        state.gravityModifier = value;
        rejectsWithoutMutation(entity, state);
        state = original;
        state.floorFriction = value;
        rejectsWithoutMutation(entity, state);
        for (int component = 0; component < 4; ++component) {
            state = original;
            state.renderTint[component] = value;
            rejectsWithoutMutation(entity, state);
        }
    }
}

TEST_CASE(EntitySimulationState_rejects_invalid_bounds_identity_and_tags) {
    Entity entity;
    const auto original = entity.simulationState();
    for (int axis = 0; axis < 3; ++axis) {
        auto state = original;
        state.localBounds.min[axis] = state.localBounds.max[axis];
        rejectsWithoutMutation(entity, state);
        state.localBounds.min[axis] += 1;
        rejectsWithoutMutation(entity, state);
        state = original;
        state.position[axis] = std::numeric_limits<float>::max();
        state.localBounds.max[axis] = std::numeric_limits<float>::max();
        rejectsWithoutMutation(entity, state);
    }
    auto state = original;
    state.typeId = "rigel:other_rule";
    rejectsWithoutMutation(entity, state);
    state = original;
    state.tags = {"duplicate", "duplicate"};
    rejectsWithoutMutation(entity, state);
    state.tags = {"z", "a"};
    rejectsWithoutMutation(entity, state);
}
