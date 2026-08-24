#pragma once

#include "Rigel/Voxel/ChunkTasks.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

namespace Rigel::Voxel::detail {

struct ThreadPoolTestAccess {
    static void construct(
        size_t threadCount,
        std::function<std::thread(std::function<void()>)> startThread) {
        ThreadPool pool(threadCount, std::move(startThread));
    }

    static constexpr bool pendingJobsAreMovable() {
        return std::is_move_constructible_v<ThreadPool::PendingJob>;
    }

    static void gateNextEnqueueReturn(ThreadPool& pool,
                                      std::atomic<bool>& entered,
                                      std::atomic<bool>& released) {
        std::lock_guard<std::mutex> lock(pool.m_mutex);
        pool.m_nextEnqueueReturnEntered = &entered;
        pool.m_nextEnqueueReturnReleased = &released;
    }
};

} // namespace Rigel::Voxel::detail
