#include "Rigel/Core/Profiler.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace Rigel::Core {

namespace {

#if defined(RIGEL_ENABLE_PROFILER)

using Clock = std::chrono::steady_clock;

struct ProfilerState {
    bool enabled = false;
    bool frameOpen = false;
    size_t maxFrames = 240;
    size_t maxRecords = 256;
    size_t cursor = 0;
    size_t filled = 0;
    size_t dropped = 0;
    std::vector<ProfilerFrame> frames;
    ProfilerFrame* current = nullptr;
};

ProfilerState& state() {
    static ProfilerState instance;
    return instance;
}

thread_local uint16_t g_depth = 0;

uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

uint32_t threadId() {
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void ensureFrames(ProfilerState& profiler) {
    if (!profiler.frames.empty()) {
        return;
    }
    profiler.frames.resize(profiler.maxFrames);
    for (auto& frame : profiler.frames) {
        frame.records.reserve(profiler.maxRecords);
    }
}

#endif

} // namespace

const ProfilerFrame* ProfilerTimelineView::latest() const {
    if (!frames || capacity == 0 || count == 0) {
        return nullptr;
    }
    size_t index = (cursor + capacity - 1) % capacity;
    return &frames[index];
}

void Profiler::setEnabled(bool enabled) {
#if defined(RIGEL_ENABLE_PROFILER)
    auto& profiler = state();
    profiler.enabled = enabled;
    profiler.frameOpen = false;
    profiler.current = nullptr;
    if (!enabled) {
        profiler.cursor = 0;
        profiler.filled = 0;
        profiler.dropped = 0;
        for (auto& frame : profiler.frames) {
            frame.records.clear();
            frame.droppedRecords = 0;
            frame.frameStartNs = 0;
            frame.frameEndNs = 0;
        }
    }
#else
    (void)enabled;
#endif
}

bool Profiler::enabled() {
#if defined(RIGEL_ENABLE_PROFILER)
    return state().enabled;
#else
    return false;
#endif
}

bool Profiler::frameOpen() {
#if defined(RIGEL_ENABLE_PROFILER)
    return state().frameOpen;
#else
    return false;
#endif
}

uint64_t Profiler::timestampNs() {
#if defined(RIGEL_ENABLE_PROFILER)
    return nowNs();
#else
    return 0;
#endif
}

uint16_t Profiler::pushDepth() {
#if defined(RIGEL_ENABLE_PROFILER)
    return g_depth++;
#else
    return 0;
#endif
}

void Profiler::popDepth() {
#if defined(RIGEL_ENABLE_PROFILER)
    if (g_depth > 0) {
        --g_depth;
    }
#endif
}

void Profiler::recordScope(const char* name, uint64_t startNs, uint64_t endNs, uint16_t depth) {
#if defined(RIGEL_ENABLE_PROFILER)
    auto& profiler = state();
    if (!profiler.enabled || !profiler.frameOpen || !profiler.current) {
        return;
    }
    if (profiler.current->records.size() >= profiler.maxRecords) {
        ++profiler.current->droppedRecords;
        ++profiler.dropped;
        return;
    }
    profiler.current->records.push_back(ProfilerRecord{ name, startNs, endNs, depth, threadId() });
#else
    (void)name;
    (void)startNs;
    (void)endNs;
    (void)depth;
#endif
}

void Profiler::beginFrame() {
#if defined(RIGEL_ENABLE_PROFILER)
    auto& profiler = state();
    if (!profiler.enabled) {
        return;
    }
    ensureFrames(profiler);
    g_depth = 0;
    profiler.current = &profiler.frames[profiler.cursor];
    profiler.current->records.clear();
    profiler.current->droppedRecords = 0;
    profiler.current->frameStartNs = nowNs();
    profiler.current->frameEndNs = 0;
    profiler.frameOpen = true;
    profiler.dropped = 0;
#else
    return;
#endif
}

void Profiler::endFrame() {
#if defined(RIGEL_ENABLE_PROFILER)
    auto& profiler = state();
    if (!profiler.enabled || !profiler.frameOpen || !profiler.current) {
        return;
    }
    profiler.current->frameEndNs = nowNs();
    profiler.frameOpen = false;
    profiler.current = nullptr;

    profiler.cursor = (profiler.cursor + 1) % profiler.maxFrames;
    profiler.filled = std::min(profiler.maxFrames, profiler.filled + 1);
#else
    return;
#endif
}

ProfilerTimelineView Profiler::getTimeline() {
#if defined(RIGEL_ENABLE_PROFILER)
    auto& profiler = state();
    if (profiler.frames.empty()) {
        return {};
    }
    return ProfilerTimelineView{
        profiler.frames.data(),
        profiler.frames.size(),
        profiler.filled,
        profiler.cursor
    };
#else
    return {};
#endif
}

const ProfilerFrame* Profiler::getLastFrame() {
#if defined(RIGEL_ENABLE_PROFILER)
    return getTimeline().latest();
#else
    return nullptr;
#endif
}

size_t Profiler::getDroppedCount() {
#if defined(RIGEL_ENABLE_PROFILER)
    return state().dropped;
#else
    return 0;
#endif
}

ProfilerScope::ProfilerScope(const char* name)
#if defined(RIGEL_ENABLE_PROFILER)
    : m_name(name)
    , m_startNs(0)
    , m_depth(0)
    , m_active(false)
#endif
{
#if defined(RIGEL_ENABLE_PROFILER)
    if (!Profiler::enabled() || !Profiler::frameOpen()) {
        return;
    }
    m_startNs = Profiler::timestampNs();
    m_depth = Profiler::pushDepth();
    m_active = true;
#else
    (void)name;
#endif
}

ProfilerScope::~ProfilerScope() {
#if defined(RIGEL_ENABLE_PROFILER)
    if (!m_active) {
        return;
    }
    uint64_t endNs = Profiler::timestampNs();
    Profiler::popDepth();
    Profiler::recordScope(m_name, m_startNs, endNs, m_depth);
#endif
}

} // namespace Rigel::Core
