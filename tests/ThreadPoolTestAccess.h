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

    static size_t pendingJobCount(ThreadPool& pool) {
        std::lock_guard<std::mutex> lock(pool.m_mutex);
        return pool.m_highPriorityJobs.size() + pool.m_jobs.size();
    }

    static ThreadPool::JobId jobId(const ThreadPool::JobHandle& handle) {
        return handle.m_id;
    }

    static bool sameIncarnation(const ThreadPool::JobHandle& lhs,
                                const ThreadPool::JobHandle& rhs) {
        return lhs.m_incarnation == rhs.m_incarnation;
    }

    static void gateNextEnqueueReturn(ThreadPool& pool,
                                      std::atomic<bool>& entered,
                                      std::atomic<bool>& released) {
        std::lock_guard<std::mutex> lock(pool.m_mutex);
        pool.m_nextEnqueueReturnEntered = &entered;
        pool.m_nextEnqueueReturnReleased = &released;
    }

    static void gateNextSubmissionCommit(ThreadPool& pool,
                                         std::atomic<bool>& entered,
                                         std::atomic<bool>& released) {
        std::lock_guard<std::mutex> lock(pool.m_mutex);
        pool.m_nextSubmissionCommitEntered = &entered;
        pool.m_nextSubmissionCommitReleased = &released;
    }
};

} // namespace Rigel::Voxel::detail
