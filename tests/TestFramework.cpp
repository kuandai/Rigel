#include "TestFramework.h"

namespace Rigel::Test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void registerTest(const char* name, void (*fn)()) {
    registry().push_back(TestCase{name, fn});
}

} // namespace Rigel::Test
