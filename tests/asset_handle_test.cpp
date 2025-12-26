#include "TestFramework.h"

#include "Rigel/Asset/Handle.h"

using namespace Rigel::Asset;

TEST_CASE(AssetHandle_Basics) {
    auto ptr = std::make_shared<int>(42);
    Handle<int> handle(ptr, "id");

    CHECK(handle);
    CHECK_EQ(*handle, 42);
    CHECK_EQ(handle.id(), "id");

    Handle<int> other(ptr, "id");
    CHECK(handle == other);
}
