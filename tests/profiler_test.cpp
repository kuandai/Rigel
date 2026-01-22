#include "TestFramework.h"

#include "Rigel/Core/Profiler.h"

#include <string>

using Rigel::Core::Profiler;
using Rigel::Core::ProfilerFrame;

TEST_CASE(Profiler_Disabled_NoRecords) {
    Profiler::setEnabled(false);
    Profiler::beginFrame();
    {
        PROFILE_SCOPE("DisabledScope");
    }
    Profiler::endFrame();

    const ProfilerFrame* frame = Profiler::getLastFrame();
    CHECK(frame == nullptr || frame->records.empty());
}

TEST_CASE(Profiler_Enabled_RecordsScope) {
    Profiler::setEnabled(true);
    Profiler::beginFrame();
    {
        PROFILE_SCOPE("EnabledScope");
    }
    Profiler::endFrame();

    const ProfilerFrame* frame = Profiler::getLastFrame();
    CHECK(frame != nullptr);
    CHECK(!frame->records.empty());

    Profiler::setEnabled(false);
}

TEST_CASE(Profiler_NestedScopes_Depth) {
    Profiler::setEnabled(true);
    Profiler::beginFrame();
    {
        PROFILE_SCOPE("Outer");
        {
            PROFILE_SCOPE("Inner");
        }
    }
    Profiler::endFrame();

    const ProfilerFrame* frame = Profiler::getLastFrame();
    CHECK(frame != nullptr);
    bool sawOuter = false;
    bool sawInner = false;
    for (const auto& record : frame->records) {
        if (record.name && std::string(record.name) == "Outer") {
            sawOuter = (record.depth == 0);
        }
        if (record.name && std::string(record.name) == "Inner") {
            sawInner = (record.depth == 1);
        }
    }
    CHECK(sawOuter);
    CHECK(sawInner);

    Profiler::setEnabled(false);
}
