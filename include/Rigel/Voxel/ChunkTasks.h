#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Rigel::Voxel::detail {

struct ThreadPoolTestAccess;

template <typename T>
class ConcurrentQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(value));
    }

    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<T> m_queue;
};

class ThreadPool {
public:
    using JobId = uint64_t;

    enum class Priority : uint8_t {
        Normal,
        High
    };

    explicit ThreadPool(size_t threadCount)
        : ThreadPool(
              threadCount,
              [](std::function<void()> worker) {
                  return std::thread(std::move(worker));
              }) {}

    ~ThreadPool() {
        stop();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    JobId enqueue(std::function<void()> job,
                  Priority priority = Priority::Normal) {
        JobId id = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return 0;
            }
            id = m_nextJobId++;
            if (m_nextJobId == 0) {
                m_nextJobId = 1;
            }
            PendingJob pending{id, std::move(job)};
            if (priority == Priority::High) {
                m_highPriorityJobs.push_back(std::move(pending));
            } else {
                m_jobs.push_back(std::move(pending));
            }
        }
        m_cv.notify_one();
        return id;
    }

    bool cancel(JobId id) {
        PendingJob cancelled;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            found = takePendingJob(m_highPriorityJobs, id, cancelled) ||
                takePendingJob(m_jobs, id, cancelled);
        }
        return found;
    }

    bool promote(JobId id) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = findPendingJob(m_jobs, id);
            if (it == m_jobs.end()) {
                return findPendingJob(m_highPriorityJobs, id) !=
                    m_highPriorityJobs.end();
            }
            m_highPriorityJobs.push_back(std::move(*it));
            m_jobs.erase(it);
        }
        m_cv.notify_one();
        return true;
    }

    size_t threadCount() const {
        return m_threads.size();
    }

    void stop(std::function<void()> onStopping = {}) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
        }
        if (onStopping) {
            onStopping();
        }
        m_cv.notify_all();
        for (std::thread& thread : m_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        m_threads.clear();
    }

private:
    struct PendingJob {
        JobId id = 0;
        std::function<void()> run;
    };

    using JobQueue = std::deque<PendingJob>;

    static JobQueue::iterator findPendingJob(JobQueue& queue, JobId id) {
        return std::find_if(
            queue.begin(), queue.end(),
            [id](const PendingJob& job) { return job.id == id; });
    }

    static bool takePendingJob(JobQueue& queue,
                               JobId id,
                               PendingJob& out) {
        auto it = findPendingJob(queue, id);
        if (it == queue.end()) {
            return false;
        }
        out.id = it->id;
        out.run.swap(it->run);
        queue.erase(it);
        return true;
    }

    using ThreadStarter =
        std::function<std::thread(std::function<void()>)>;

    ThreadPool(size_t threadCount, ThreadStarter startThread) {
        if (threadCount == 0) {
            return;
        }
        m_threads.reserve(threadCount);
        try {
            for (size_t i = 0; i < threadCount; ++i) {
                m_threads.emplace_back(
                    startThread([this]() { workerLoop(); }));
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    friend struct ThreadPoolTestAccess;

    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return m_stopping || !m_highPriorityJobs.empty() ||
                        !m_jobs.empty();
                });
                if (m_stopping && m_highPriorityJobs.empty() &&
                    m_jobs.empty()) {
                    return;
                }
                JobQueue& queue = m_highPriorityJobs.empty()
                    ? m_jobs
                    : m_highPriorityJobs;
                job = std::move(queue.front().run);
                queue.pop_front();
            }
            try {
                job();
            } catch (...) {
                // Typed jobs are responsible for publishing failure results.
                // Keep an unexpected worker exception from terminating the process.
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    JobQueue m_highPriorityJobs;
    JobQueue m_jobs;
    std::vector<std::thread> m_threads;
    JobId m_nextJobId = 1;
    bool m_stopping = false;
};

} // namespace Rigel::Voxel::detail
