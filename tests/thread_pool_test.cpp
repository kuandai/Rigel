#include "TestFramework.h"

#include "Rigel/Voxel/ChunkTasks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
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

    static constexpr bool pendingJobsAreMovable() {
        return std::is_move_constructible_v<ThreadPool::PendingJob>;
    }
};
}

static_assert(
    !Rigel::Voxel::detail::ThreadPoolTestAccess::pendingJobsAreMovable());

namespace {

class ThreadStartError : public std::runtime_error {
public:
    ThreadStartError() : std::runtime_error("thread start failed") {}
};

class ExpectedFixtureError : public std::runtime_error {
public:
    ExpectedFixtureError() : std::runtime_error("expected fixture failure") {}
};

class ThreadPoolGate {
public:
    void enterAndWait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this]() { return m_released; });
    }

    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_entered; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_entered = false;
    bool m_released = false;
};

class ThreadPoolRelease {
public:
    explicit ThreadPoolRelease(ThreadPoolGate& gate) : m_gate(gate) {}

    ~ThreadPoolRelease() {
        m_gate.release();
    }

private:
    ThreadPoolGate& m_gate;
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

TEST_CASE(ConcurrentQueue_PublicationCallbackCompletesBeforeDrain) {
    Rigel::Voxel::detail::ConcurrentQueue<int> queue;
    ThreadPoolGate publicationGate;
    ThreadPoolRelease releaseOnExit(publicationGate);
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> drained{0};
    std::atomic<bool> drainAttempted{false};
    std::atomic<bool> drainReturned{false};
    bool popped = false;
    int value = 0;

    std::thread producer([&]() {
        queue.push(
            42,
            [&]() noexcept {
                published.fetch_add(1, std::memory_order_seq_cst);
                publicationGate.enterAndWait();
            });
    });
    const bool publicationEntered = publicationGate.waitUntilEntered();

    std::thread consumer([&]() {
        drainAttempted.store(true, std::memory_order_release);
        popped = queue.tryPop(value);
        if (popped) {
            drained.fetch_add(1, std::memory_order_seq_cst);
        }
        drainReturned.store(true, std::memory_order_release);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!drainAttempted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool consumerReachedQueue =
        drainAttempted.load(std::memory_order_acquire);
    const bool drainBlockedDuringPublication =
        !drainReturned.load(std::memory_order_acquire);

    publicationGate.release();
    producer.join();
    consumer.join();

    CHECK(publicationEntered);
    CHECK(consumerReachedQueue);
    CHECK(drainBlockedDuringPublication);
    CHECK(popped);
    CHECK_EQ(value, 42);
    CHECK_EQ(published.load(std::memory_order_seq_cst),
             static_cast<uint64_t>(1));
    CHECK_EQ(drained.load(std::memory_order_seq_cst),
             static_cast<uint64_t>(1));
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
    ThreadPoolGate blocker;
    std::vector<int> order;
    Rigel::Voxel::detail::ThreadPool pool(1);
    ThreadPoolRelease releaseOnExit(blocker);

    pool.enqueue([&]() {
        blocker.enterAndWait();
    });
    const bool started = blocker.waitUntilEntered();

    const auto normal = pool.enqueue([&]() { order.push_back(1); });
    const auto cancelled = pool.enqueue([&]() { order.push_back(2); });
    const auto promoted = pool.enqueue([&]() { order.push_back(3); });
    const bool cancelledPending = pool.cancel(cancelled);
    const bool cancelledTwice = pool.cancel(cancelled);
    const bool promotedPending = pool.promote(promoted);

    blocker.release();
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

TEST_CASE(ThreadPool_CancelDestroysCallableAfterUnlock) {
    std::atomic<size_t> destroyed = 0;
    std::atomic<size_t> reentrantRuns = 0;
    std::atomic<bool> reentrantCancelled = false;

    struct ReentrantCapture {
        Rigel::Voxel::detail::ThreadPool* pool = nullptr;
        std::atomic<size_t>* destroyed = nullptr;
        std::atomic<size_t>* runs = nullptr;
        std::atomic<bool>* cancelled = nullptr;

        ~ReentrantCapture() {
            destroyed->fetch_add(1, std::memory_order_relaxed);
            const auto id = pool->enqueue([runs = runs]() {
                runs->fetch_add(1, std::memory_order_relaxed);
            });
            cancelled->store(pool->cancel(id), std::memory_order_relaxed);
        }
    };

    auto capture = std::make_shared<ReentrantCapture>();
    Rigel::Voxel::detail::ThreadPool pool(0);
    capture->pool = &pool;
    capture->destroyed = &destroyed;
    capture->runs = &reentrantRuns;
    capture->cancelled = &reentrantCancelled;

    const auto cancelledJob = pool.enqueue([capture]() {});
    capture.reset();

    CHECK(pool.cancel(cancelledJob));
    CHECK_EQ(destroyed.load(std::memory_order_relaxed), static_cast<size_t>(1));
    CHECK(reentrantCancelled.load(std::memory_order_relaxed));
    CHECK_EQ(reentrantRuns.load(std::memory_order_relaxed), static_cast<size_t>(0));
    CHECK(!pool.cancel(cancelledJob));
    CHECK_NO_THROW(pool.stop());
    CHECK_EQ(pool.enqueue([]() {}),
             Rigel::Voxel::detail::ThreadPool::JobId{0});
}

TEST_CASE(ThreadPool_PromotionRetainsReentrantCallableUntilUnlockedRemoval) {
    struct ReentrantState {
        Rigel::Voxel::detail::ThreadPool* pool = nullptr;
        std::atomic<bool> armed = false;
        std::atomic<size_t> destroyed = 0;
        std::atomic<bool> cancelled = false;
    };

    struct ReentrantCallable {
        std::shared_ptr<ReentrantState> state;

        explicit ReentrantCallable(std::shared_ptr<ReentrantState> value)
            : state(std::move(value)) {}
        ReentrantCallable(const ReentrantCallable&) = default;
        ReentrantCallable(ReentrantCallable&& other) noexcept
            : state(other.state) {}

        void operator()() const {}

        ~ReentrantCallable() {
            if (!state || !state->armed.load(std::memory_order_relaxed)) {
                return;
            }
            state->destroyed.fetch_add(1, std::memory_order_relaxed);
            const auto id = state->pool->enqueue([]() {});
            state->cancelled.store(
                state->pool->cancel(id), std::memory_order_relaxed);
        }
    };

    Rigel::Voxel::detail::ThreadPool pool(0);
    auto state = std::make_shared<ReentrantState>();
    state->pool = &pool;
    const auto job = pool.enqueue(ReentrantCallable{state});
    state->armed.store(true, std::memory_order_relaxed);

    CHECK(pool.promote(job));
    CHECK_EQ(state->destroyed.load(std::memory_order_relaxed),
             static_cast<size_t>(0));
    CHECK(pool.cancel(job));
    CHECK_EQ(state->destroyed.load(std::memory_order_relaxed),
             static_cast<size_t>(1));
    CHECK(state->cancelled.load(std::memory_order_relaxed));
    CHECK(!pool.cancel(job));
}

TEST_CASE(ThreadPool_GatedFixtureUnwindsAfterExpectedException) {
    bool exceptionReachedCaller = false;
    try {
        ThreadPoolGate blocker;
        Rigel::Voxel::detail::ThreadPool pool(1);
        ThreadPoolRelease releaseOnExit(blocker);
        pool.enqueue([&blocker]() { blocker.enterAndWait(); });
        CHECK(blocker.waitUntilEntered());
        throw ExpectedFixtureError();
    } catch (const ExpectedFixtureError&) {
        exceptionReachedCaller = true;
    }
    CHECK(exceptionReachedCaller);
}
