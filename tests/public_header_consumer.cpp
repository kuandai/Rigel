#include <Rigel/Asset/Assets.h>
#include <Rigel/Preferences/UserPreferences.h>
#include <Rigel/Render/TemporalJitter.h>
#include <Rigel/Util/Yaml.h>
#include <Rigel/Voxel/WorldView.h>
#include <Rigel/input/InputState.h>

uint64_t consumeRegionSchedulerBenchmark(
    const Rigel::Voxel::WorldView& view) {
    const auto& regions = view.streamingDiagnostics().regionScheduler;
    const auto consumeOrigin = [](const auto& origin) {
        return origin.logicalAdmissions + origin.retryAdmissions +
            origin.logicalPreStartCancellations + origin.poolSubmissions +
            origin.poolResubmissions + origin.successfulPoolYields +
            origin.terminalPoolCancellations + origin.poolWorkerStarts +
            origin.inlineExecutions + origin.resultsPublished +
            origin.resultsDrained + origin.missingProbes +
            origin.admissionToWorkerStartNanoseconds +
            origin.maxAdmissionToWorkerStartNanoseconds +
            origin.workerExecutionNanoseconds +
            origin.maxWorkerExecutionNanoseconds;
    };
    const uint64_t benchmarkValue =
        consumeOrigin(regions.directOrigin) +
        consumeOrigin(regions.speculativeOrigin) +
        regions.demandPromotions + regions.usefulPrefetchCacheHits +
        regions.speculativeEvictionsBeforeDemand +
        regions.demandOwnedQueued + regions.speculativeOwnedQueued +
        regions.demandOwnedDispatchedUndrained +
        regions.speculativeOwnedDispatchedUndrained +
        regions.speculativePoolJobsPending +
        regions.maxSpeculativePoolJobsPending +
        regions.speculativePoolYieldCalls +
        regions.speculativePoolYieldCandidateVisits +
        regions.maxSpeculativePoolYieldCandidateVisits;
    return benchmarkValue;
}

int main() {
    Rigel::Preferences::UserPreferences preferences;
    (void)preferences.graphics.viewDistanceChunks;
    Rigel::Render::TemporalJitterSequence jitter;
    (void)jitter.next(1280, 720, 1.0f);
    const auto consumer = &consumeRegionSchedulerBenchmark;
    return consumer ? 0 : 1;
}
