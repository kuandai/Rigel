#include "TestFramework.h"

#include "Rigel/Core/FramePacer.h"

#include <optional>
#include <vector>

namespace {

struct FakeClock {
    double now = 0.0;
    std::vector<double> deadlines;
};

FakeClock* g_clock = nullptr;

double now() {
    return g_clock->now;
}

void sleepUntil(double deadline) {
    g_clock->deadlines.push_back(deadline);
    g_clock->now = deadline;
}

Rigel::Core::FramePacer makePacer(FakeClock& clock) {
    g_clock = &clock;
    return Rigel::Core::FramePacer({&now, &sleepUntil});
}

} // namespace

TEST_CASE(FramePacer_UsesAbsoluteDeadlinesWithoutDrift) {
    FakeClock clock;
    auto pacer = makePacer(clock);
    pacer.setLimit(100);

    pacer.wait();
    clock.now += 0.004;
    pacer.wait();
    clock.now += 0.007;
    pacer.wait();

    CHECK_EQ(clock.deadlines.size(), static_cast<size_t>(3));
    CHECK_NEAR(clock.deadlines[0], 0.01, 0.000001);
    CHECK_NEAR(clock.deadlines[1], 0.02, 0.000001);
    CHECK_NEAR(clock.deadlines[2], 0.03, 0.000001);
}

TEST_CASE(FramePacer_SkipsMissedPeriodsAfterAStall) {
    FakeClock clock;
    auto pacer = makePacer(clock);
    pacer.setLimit(50);

    pacer.wait();
    clock.now = 0.095;
    pacer.wait();
    pacer.wait();

    CHECK_EQ(clock.deadlines.size(), static_cast<size_t>(2));
    CHECK_NEAR(clock.deadlines[0], 0.02, 0.000001);
    CHECK_NEAR(clock.deadlines[1], 0.10, 0.000001);
}

TEST_CASE(FramePacer_LimitChangesAndUnlimitedResetTheSchedule) {
    FakeClock clock;
    auto pacer = makePacer(clock);
    pacer.setLimit(60);
    pacer.wait();

    clock.now = 1.0;
    pacer.setLimit(std::nullopt);
    pacer.wait();
    CHECK_EQ(clock.deadlines.size(), static_cast<size_t>(1));

    pacer.setLimit(30);
    pacer.wait();
    CHECK_EQ(clock.deadlines.size(), static_cast<size_t>(2));
    CHECK_NEAR(clock.deadlines.back(), 1.0 + 1.0 / 30.0, 0.000001);
    CHECK_THROWS(pacer.setLimit(0));
}
