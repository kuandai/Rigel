#include "Rigel/Entity/EntityId.h"

#include <atomic>
#include <chrono>
#include <random>

namespace Rigel::Entity {

EntityId EntityId::New() {
    static std::atomic<uint32_t> counter{1};
    static std::mt19937 rng{std::random_device{}()};
    static std::uniform_int_distribution<uint32_t> dist;

    EntityId id;
    id.time = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    id.random = dist(rng);
    id.counter = counter.fetch_add(1, std::memory_order_relaxed);
    return id;
}

} // namespace Rigel::Entity
