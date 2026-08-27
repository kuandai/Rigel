#pragma once

#include <optional>

namespace Rigel::Core {

class FramePacer final {
public:
    struct Clock {
        double (*now)();
        void (*sleepUntil)(double deadlineSeconds);
    };

    FramePacer();
    explicit FramePacer(Clock clock);

    void setLimit(std::optional<int> framesPerSecond);

    void wait();
    void reset();
    double now() const;

private:
    Clock m_clock;
    std::optional<int> m_limit;
    std::optional<double> m_nextDeadline;
};

} // namespace Rigel::Core
