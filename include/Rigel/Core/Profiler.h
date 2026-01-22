#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace Rigel::Core {

struct ProfilerRecord {
    const char* name = nullptr;
    uint64_t startNs = 0;
    uint64_t endNs = 0;
    uint16_t depth = 0;
    uint32_t threadId = 0;
};

struct ProfilerFrame {
    uint64_t frameStartNs = 0;
    uint64_t frameEndNs = 0;
    std::vector<ProfilerRecord> records;
    size_t droppedRecords = 0;
};

struct ProfilerTimelineView {
    const ProfilerFrame* frames = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    size_t cursor = 0;

    const ProfilerFrame* latest() const;
};

class Profiler {
public:
    static void setEnabled(bool enabled);
    static bool enabled();

    static void beginFrame();
    static void endFrame();

    static ProfilerTimelineView getTimeline();
    static const ProfilerFrame* getLastFrame();
    static size_t getDroppedCount();

private:
    friend class ProfilerScope;

    static uint64_t timestampNs();
    static uint16_t pushDepth();
    static void popDepth();
    static void recordScope(const char* name, uint64_t startNs, uint64_t endNs, uint16_t depth);
    static bool frameOpen();
};

class ProfilerScope {
public:
    explicit ProfilerScope(const char* name);
    ~ProfilerScope();

private:
    const char* m_name = nullptr;
    uint64_t m_startNs = 0;
    uint16_t m_depth = 0;
    bool m_active = false;
};

} // namespace Rigel::Core

#if defined(RIGEL_ENABLE_PROFILER)
#define PROFILE_SCOPE(name) ::Rigel::Core::ProfilerScope profilerScope_##__LINE__(name)
#else
#define PROFILE_SCOPE(name) do { (void)sizeof(name); } while (false)
#endif
