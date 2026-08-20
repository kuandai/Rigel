#include "TestFramework.h"

#include "Rigel/Core/Profiler.h"

using Rigel::Core::Profiler;
using Rigel::Core::ProfilerScope;

TEST_CASE(Profiler_RigelLibMatchesBuildConfiguration) {
    Profiler::setEnabled(true);

#if defined(RIGEL_EXPECT_PROFILER_ENABLED)
    CHECK(Profiler::enabled());

    Profiler::beginFrame();
    {
        ProfilerScope scope("RigelLibScope");
    }
    Profiler::endFrame();

    const auto* frame = Profiler::getLastFrame();
    CHECK(frame != nullptr);
    CHECK(!frame->records.empty());
#else
    CHECK(!Profiler::enabled());

    Profiler::beginFrame();
    {
        ProfilerScope scope("RigelLibScope");
    }
    Profiler::endFrame();

    CHECK(Profiler::getLastFrame() == nullptr);
#endif

    Profiler::setEnabled(false);
}
