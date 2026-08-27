#include "Rigel/Core/FramePacer.h"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace Rigel::Core {
namespace {

using SteadyClock = std::chrono::steady_clock;

double steadyNow() {
    return std::chrono::duration<double>(
               SteadyClock::now().time_since_epoch())
        .count();
}

void steadySleepUntil(double deadlineSeconds) {
    const auto deadline = SteadyClock::time_point(
        std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(deadlineSeconds)));
    std::this_thread::sleep_until(deadline);
}

} // namespace

FramePacer::FramePacer()
    : FramePacer(Clock{&steadyNow, &steadySleepUntil}) {
}

FramePacer::FramePacer(Clock clock)
    : m_clock(clock) {
}

void FramePacer::setLimit(std::optional<int> framesPerSecond) {
    if (framesPerSecond && *framesPerSecond <= 0) {
        throw std::invalid_argument("Frame limit must be positive");
    }
    if (m_limit == framesPerSecond) {
        return;
    }
    m_limit = framesPerSecond;
    reset();
}

void FramePacer::wait() {
    if (!m_limit) {
        m_nextDeadline.reset();
        return;
    }

    const double period = 1.0 / static_cast<double>(*m_limit);
    const double beforeSleep = now();
    if (!m_nextDeadline) {
        m_nextDeadline = beforeSleep + period;
    }

    if (beforeSleep < *m_nextDeadline) {
        m_clock.sleepUntil(*m_nextDeadline);
    }

    const double afterSleep = now();
    *m_nextDeadline += period;
    if (afterSleep >= *m_nextDeadline) {
        const double missedPeriods =
            std::floor((afterSleep - *m_nextDeadline) / period) + 1.0;
        *m_nextDeadline += missedPeriods * period;
    }
}

void FramePacer::reset() {
    m_nextDeadline.reset();
}

double FramePacer::now() const {
    return m_clock.now();
}

} // namespace Rigel::Core
