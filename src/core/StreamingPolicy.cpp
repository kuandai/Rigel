#include "StreamingPolicy.h"

#include <algorithm>
#include <thread>

namespace Rigel::detail {

namespace {

constexpr unsigned kFallbackLogicalProcessorCount = 4;
constexpr unsigned kMaximumLogicalProcessorCount = 64;
constexpr int kMaximumSchedulerWorkerThreads = 12;
constexpr int kMaximumIoThreads = 2;
constexpr int kMaximumPayloadWorkerThreads = 4;

} // namespace

StreamingPolicy makeAutomaticStreamingPolicy() {
    return makeAutomaticStreamingPolicy(std::thread::hardware_concurrency());
}

StreamingPolicy makeAutomaticStreamingPolicy(
    unsigned logicalProcessorCount) {
    const unsigned detectedProcessors = logicalProcessorCount == 0
        ? kFallbackLogicalProcessorCount
        : std::max(logicalProcessorCount, kFallbackLogicalProcessorCount);
    const unsigned processors = std::min(
        detectedProcessors, kMaximumLogicalProcessorCount);

    const int ioThreads = processors >= 8 ? kMaximumIoThreads : 1;
    const int payloadThreads = std::clamp(
        static_cast<int>(processors / 5),
        1,
        kMaximumPayloadWorkerThreads);
    const int schedulerThreads = std::clamp(
        static_cast<int>(processors) - ioThreads - payloadThreads,
        2,
        kMaximumSchedulerWorkerThreads);

    StreamingPolicy policy;
    policy.streamer.genQueueLimit = 128;
    policy.streamer.meshQueueLimit = 128;
    policy.streamer.updateBudgetPerFrame = 4096;
    policy.streamer.applyBudgetPerFrame = 128;
    policy.streamer.workerThreads = schedulerThreads;
    policy.streamer.loadApplyBudgetPerFrame = 16;
    policy.streamer.maxResidentChunks = 0;
    policy.ioThreads = static_cast<size_t>(ioThreads);
    policy.loadWorkerThreads = static_cast<size_t>(payloadThreads);
    return policy;
}

} // namespace Rigel::detail
