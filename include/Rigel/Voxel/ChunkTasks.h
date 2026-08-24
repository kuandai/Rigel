#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Rigel::Voxel::detail {

struct ThreadPoolTestAccess;
struct ConcurrentQueueTestAccess;

template <typename T>
class ConcurrentQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(value));
    }

    template <typename OnPublished>
    void push(T value, OnPublished&& onPublished) {
        static_assert(std::is_nothrow_invocable_v<OnPublished&>);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(value));
        std::invoke(onPublished);
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
    friend struct ConcurrentQueueTestAccess;

    mutable std::mutex m_mutex;
    std::deque<T> m_queue;
};

class ThreadPool {
    struct Incarnation;

public:
    using JobId = uint64_t;

    class JobHandle {
    public:
        JobHandle() noexcept = default;

        explicit operator bool() const noexcept {
            return m_id != 0 && m_incarnation != nullptr;
        }

    private:
        friend class ThreadPool;
        friend struct ThreadPoolTestAccess;

        JobHandle(JobId id,
                  std::shared_ptr<const Incarnation> incarnation) noexcept
            : m_id(id),
              m_incarnation(std::move(incarnation)) {}

        JobId m_id = 0;
        // Keeps a retired pool distinct even if a replacement reuses both
        // its storage address and its pool-local job ID.
        std::shared_ptr<const Incarnation> m_incarnation;
    };

    // Counter-only publication performed while the queued job is still
    // protected by the pool mutex. This deliberately cannot run user code.
    class SubmissionCommitAccounting {
    public:
        SubmissionCommitAccounting() noexcept = default;

        explicit SubmissionCommitAccounting(
            std::atomic<uint64_t>& submissions,
            std::atomic<uint64_t>* subset = nullptr) noexcept
            : m_submissions(&submissions),
              m_subset(subset) {}

    private:
        friend class ThreadPool;

        void commit(std::atomic<bool>* firstPublicationEntered,
                    std::atomic<bool>* firstPublicationReleased)
            const noexcept {
            // Readers load the subset first, so publish the total first.
            if (m_submissions) {
                m_submissions->fetch_add(1, std::memory_order_seq_cst);
            }
            if (m_subset && firstPublicationEntered &&
                firstPublicationReleased) {
                firstPublicationEntered->store(true, std::memory_order_release);
                firstPublicationEntered->notify_all();
                while (!firstPublicationReleased->load(
                    std::memory_order_acquire)) {
                    firstPublicationReleased->wait(
                        false, std::memory_order_acquire);
                }
            }
            if (m_subset) {
                m_subset->fetch_add(1, std::memory_order_seq_cst);
            }
        }

        std::atomic<uint64_t>* m_submissions = nullptr;
        std::atomic<uint64_t>* m_subset = nullptr;
    };

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

    JobId enqueue(
        std::function<void()> job,
        Priority priority = Priority::Normal,
        SubmissionCommitAccounting accounting = {}) {
        JobId id = 0;
        std::atomic<bool>* enqueueReturnEntered = nullptr;
        std::atomic<bool>* enqueueReturnReleased = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return 0;
            }
            id = m_nextJobId++;
            if (m_nextJobId == 0) {
                m_nextJobId = 1;
            }
            if (priority == Priority::High) {
                m_highPriorityJobs.emplace_back(id);
                m_highPriorityJobs.back().run.swap(job);
            } else {
                m_jobs.emplace_back(id);
                m_jobs.back().run.swap(job);
            }
            accounting.commit(
                m_nextSubmissionCommitEntered,
                m_nextSubmissionCommitReleased);
            m_nextSubmissionCommitEntered = nullptr;
            m_nextSubmissionCommitReleased = nullptr;
            enqueueReturnEntered = m_nextEnqueueReturnEntered;
            enqueueReturnReleased = m_nextEnqueueReturnReleased;
            m_nextEnqueueReturnEntered = nullptr;
            m_nextEnqueueReturnReleased = nullptr;
        }
        m_cv.notify_one();
        if (enqueueReturnEntered && enqueueReturnReleased) {
            enqueueReturnEntered->store(true, std::memory_order_release);
            enqueueReturnEntered->notify_all();
            while (!enqueueReturnReleased->load(std::memory_order_acquire)) {
                enqueueReturnReleased->wait(false, std::memory_order_acquire);
            }
        }
        return id;
    }

    JobHandle enqueueCancellable(
        std::function<void()> job,
        Priority priority = Priority::Normal,
        SubmissionCommitAccounting accounting = {}) {
        const JobId id = enqueue(
            std::move(job), priority, std::move(accounting));
        return id == 0 ? JobHandle{} : JobHandle{id, m_incarnation};
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

    bool cancel(const JobHandle& handle) {
        if (handle.m_incarnation != m_incarnation) {
            return false;
        }
        return cancel(handle.m_id);
    }

    bool promote(JobId id) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = findPendingJob(m_jobs, id);
            if (it == m_jobs.end()) {
                return findPendingJob(m_highPriorityJobs, id) !=
                    m_highPriorityJobs.end();
            }
            m_highPriorityJobs.emplace_back(it->id);
            m_highPriorityJobs.back().run.swap(it->run);
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
    struct Incarnation {};

    struct PendingJob {
        explicit PendingJob(JobId jobId = 0) : id(jobId) {}

        PendingJob(const PendingJob&) = delete;
        PendingJob& operator=(const PendingJob&) = delete;
        PendingJob(PendingJob&&) = delete;
        PendingJob& operator=(PendingJob&& other) noexcept {
            if (this != &other) {
                id = other.id;
                run.swap(other.run);
            }
            return *this;
        }

        JobId id = 0;
        std::function<void()> run;
    };

    static_assert(!std::is_move_constructible_v<PendingJob>);

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
                job.swap(queue.front().run);
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
    // One-shot deterministic boundaries exposed only through test access.
    std::atomic<bool>* m_nextSubmissionCommitEntered = nullptr;
    std::atomic<bool>* m_nextSubmissionCommitReleased = nullptr;
    std::atomic<bool>* m_nextEnqueueReturnEntered = nullptr;
    std::atomic<bool>* m_nextEnqueueReturnReleased = nullptr;
    JobId m_nextJobId = 1;
    std::shared_ptr<const Incarnation> m_incarnation =
        std::make_shared<Incarnation>();
    bool m_stopping = false;
};

} // namespace Rigel::Voxel::detail
