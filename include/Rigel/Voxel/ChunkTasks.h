#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Rigel::Voxel::detail {

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
    explicit ThreadPool(size_t threadCount) {
        if (threadCount == 0) {
            return;
        }
        m_threads.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            m_threads.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPool() {
        stop();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_jobs.push_back(std::move(job));
        }
        m_cv.notify_one();
    }

    size_t threadCount() const {
        return m_threads.size();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
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
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return m_stopping || !m_jobs.empty(); });
                if (m_stopping && m_jobs.empty()) {
                    return;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            job();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_jobs;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};

} // namespace Rigel::Voxel::detail
