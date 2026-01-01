#include "TestFramework.h"

#include "Rigel/Entity/EntityId.h"

#include <unordered_set>

using namespace Rigel::Entity;

TEST_CASE(EntityId_Unique) {
    std::unordered_set<EntityId, EntityIdHash> ids;
    constexpr int kCount = 1024;
    for (int i = 0; i < kCount; ++i) {
        EntityId id = EntityId::New();
        CHECK(!id.isNull());
        ids.insert(id);
    }
    CHECK_EQ(ids.size(), static_cast<size_t>(kCount));
}
