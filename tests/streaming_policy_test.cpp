#include "TestFramework.h"

#include "StreamingPolicy.h"

#include <thread>

using Rigel::detail::makeAutomaticStreamingPolicy;

namespace {

void checkInternalCapacities(const Rigel::detail::StreamingPolicy& policy) {
    CHECK_EQ(policy.streamer.genQueueLimit, size_t{128});
    CHECK_EQ(policy.streamer.meshQueueLimit, size_t{128});
    CHECK_EQ(policy.streamer.updateBudgetPerFrame, 4096);
    CHECK_EQ(policy.streamer.applyBudgetPerFrame, 128);
    CHECK_EQ(policy.streamer.loadApplyBudgetPerFrame, 16);
    CHECK_EQ(policy.loadRegionDrainBudget, size_t{16});
    CHECK_EQ(policy.loadQueueLimit, size_t{0});
    CHECK_EQ(policy.loadMaxCachedRegions, size_t{16});
    CHECK_EQ(policy.loadMaxInFlightRegions, size_t{16});
    CHECK_EQ(policy.streamer.maxResidentChunks, size_t{0});
}

} // namespace

TEST_CASE(StreamingPolicy_PreservesRepresentativeTwentyProcessorTopology) {
    const auto policy = makeAutomaticStreamingPolicy(20);

    CHECK_EQ(policy.streamer.workerThreads, 12);
    CHECK_EQ(policy.ioThreads, size_t{2});
    CHECK_EQ(policy.loadWorkerThreads, size_t{4});
    checkInternalCapacities(policy);
}

TEST_CASE(StreamingPolicy_ScalesTopologyForSmallerHosts) {
    const auto fourProcessorPolicy = makeAutomaticStreamingPolicy(4);
    CHECK_EQ(fourProcessorPolicy.streamer.workerThreads, 2);
    CHECK_EQ(fourProcessorPolicy.ioThreads, size_t{1});
    CHECK_EQ(fourProcessorPolicy.loadWorkerThreads, size_t{1});

    const auto eightProcessorPolicy = makeAutomaticStreamingPolicy(8);
    CHECK_EQ(eightProcessorPolicy.streamer.workerThreads, 5);
    CHECK_EQ(eightProcessorPolicy.ioThreads, size_t{2});
    CHECK_EQ(eightProcessorPolicy.loadWorkerThreads, size_t{1});
    checkInternalCapacities(eightProcessorPolicy);
}

TEST_CASE(StreamingPolicy_UsesConservativeUnknownTopologyFallback) {
    const auto unknownPolicy = makeAutomaticStreamingPolicy(0);
    const auto fallbackPolicy = makeAutomaticStreamingPolicy(4);

    CHECK_EQ(
        unknownPolicy.streamer.workerThreads,
        fallbackPolicy.streamer.workerThreads);
    CHECK_EQ(unknownPolicy.ioThreads, fallbackPolicy.ioThreads);
    CHECK_EQ(unknownPolicy.loadWorkerThreads, fallbackPolicy.loadWorkerThreads);
    checkInternalCapacities(unknownPolicy);
}

TEST_CASE(StreamingPolicy_CapsVeryLargeHosts) {
    const auto policy = makeAutomaticStreamingPolicy(1024);

    CHECK_EQ(policy.streamer.workerThreads, 12);
    CHECK_EQ(policy.ioThreads, size_t{2});
    CHECK_EQ(policy.loadWorkerThreads, size_t{4});
    checkInternalCapacities(policy);
}

TEST_CASE(StreamingPolicy_NormalStartupUsesDetectedHostTopology) {
    const auto automatic = makeAutomaticStreamingPolicy();
    const auto expected = makeAutomaticStreamingPolicy(
        std::thread::hardware_concurrency());

    CHECK_EQ(
        automatic.streamer.workerThreads,
        expected.streamer.workerThreads);
    CHECK_EQ(automatic.ioThreads, expected.ioThreads);
    CHECK_EQ(automatic.loadWorkerThreads, expected.loadWorkerThreads);
    checkInternalCapacities(automatic);
}
