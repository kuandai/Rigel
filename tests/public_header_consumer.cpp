#include <Rigel/Asset/Assets.h>
#include <Rigel/Render/TemporalJitter.h>
#include <Rigel/Util/Yaml.h>
#include <Rigel/Voxel/WorldView.h>
#include <Rigel/input/InputState.h>

int main() {
    Rigel::Render::TemporalJitterSequence jitter;
    (void)jitter.next(1280, 720, 1.0f);
    Rigel::Voxel::StreamingDiagnosticSnapshot streaming;
    const auto& regions = streaming.regionScheduler;
    const uint64_t benchmarkValue =
        regions.directOrigin.logicalAdmissions +
        regions.directOrigin.poolSubmissions +
        regions.directOrigin.poolWorkerStarts +
        regions.directOrigin.resultsPublished +
        regions.directOrigin.resultsDrained +
        regions.directOrigin.admissionToWorkerStartNanoseconds +
        regions.speculativeOrigin.logicalAdmissions +
        regions.speculativeOrigin.successfulPoolYields +
        regions.speculativeOrigin.workerExecutionNanoseconds +
        regions.demandPromotions + regions.usefulPrefetchCacheHits;
    return benchmarkValue == 0 ? 0 : 1;
}
