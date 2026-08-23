#include "TestFramework.h"

#include "Rigel/Voxel/ChunkTasks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace Rigel::Voxel::detail {
struct ThreadPoolTestAccess {
    static void construct(
        size_t threadCount,
        std::function<std::thread(std::function<void()>)> startThread) {
        ThreadPool pool(threadCount, std::move(startThread));
    }
};
}

namespace {

class ThreadStartError : public std::runtime_error {
public:
    ThreadStartError() : std::runtime_error("thread start failed") {}
};

}

TEST_CASE(ThreadPool_ConstructionFailureJoinsStartedWorker) {
    size_t startCount = 0;
    std::atomic<bool> workerExited = false;
    bool exceptionReachedCaller = false;

    try {
        Rigel::Voxel::detail::ThreadPoolTestAccess::construct(
            2,
            [&startCount, &workerExited](std::function<void()> worker) {
                if (startCount++ == 1) {
                    throw ThreadStartError();
                }
                return std::thread(
                    [worker = std::move(worker), &workerExited]() mutable {
                        worker();
                        workerExited.store(true, std::memory_order_release);
                    });
            });
    } catch (const ThreadStartError&) {
        exceptionReachedCaller = true;
        CHECK(workerExited.load(std::memory_order_acquire));
    }

    CHECK(exceptionReachedCaller);
    CHECK_EQ(startCount, 2u);
}

TEST_CASE(ThreadPool_StopDrainsQueuedJobs) {
    std::atomic<size_t> completed = 0;
    Rigel::Voxel::detail::ThreadPool pool(2);

    for (size_t i = 0; i < 32; ++i) {
        pool.enqueue([&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.stop();

    CHECK_EQ(completed.load(std::memory_order_relaxed), 32u);
    CHECK_EQ(pool.threadCount(), 0u);
    CHECK_NO_THROW(pool.stop());
}

TEST_CASE(ThreadPool_DestructionDrainsQueuedJobs) {
    std::atomic<size_t> completed = 0;
    {
        Rigel::Voxel::detail::ThreadPool pool(1);
        for (size_t i = 0; i < 8; ++i) {
            pool.enqueue([&completed]() {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }

    CHECK_EQ(completed.load(std::memory_order_relaxed), 8u);
}

TEST_CASE(ThreadPool_PromotesAndCancelsPendingJobs) {
    Rigel::Voxel::detail::ThreadPool pool(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool blockerStarted = false;
    bool releaseBlocker = false;
    std::vector<int> order;

    pool.enqueue([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        blockerStarted = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return releaseBlocker; });
    });
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&]() { return blockerStarted; });
    }

    const auto normal = pool.enqueue([&]() { order.push_back(1); });
    const auto cancelled = pool.enqueue([&]() { order.push_back(2); });
    const auto promoted = pool.enqueue([&]() { order.push_back(3); });
    const bool cancelledPending = pool.cancel(cancelled);
    const bool cancelledTwice = pool.cancel(cancelled);
    const bool promotedPending = pool.promote(promoted);

    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseBlocker = true;
        condition.notify_all();
    }
    pool.stop();

    CHECK(started);
    CHECK(cancelledPending);
    CHECK(!cancelledTwice);
    CHECK(promotedPending);
    CHECK_EQ(order.size(), static_cast<size_t>(2));
    CHECK_EQ(order[0], 3);
    CHECK_EQ(order[1], 1);
    CHECK(!pool.cancel(normal));
}
