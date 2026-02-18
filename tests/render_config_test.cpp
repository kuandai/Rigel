#include "TestFramework.h"

#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/WorldConfigProvider.h"

using namespace Rigel::Voxel;

namespace {

class StringConfigSource : public IConfigSource {
public:
    explicit StringConfigSource(std::string yaml)
        : m_yaml(std::move(yaml))
    {}

    std::optional<std::string> load() const override {
        return m_yaml;
    }

    std::string name() const override {
        return "string";
    }

private:
    std::string m_yaml;
};

} // namespace

TEST_CASE(RenderConfig_ApplyYaml) {
    const std::string yaml = R"(
render:
  sun_direction: [0.2, 0.8, 0.1]
  transparent_alpha: 0.4
  render_distance: 300.0
  shadow:
    enabled: true
    cascades: 2
    map_size: 512
    max_distance: 150.0
    split_lambda: 0.6
    bias: 0.001
    normal_bias: 0.01
    pcf_radius: 2
    pcf_radius_near: 1
    pcf_radius_far: 3
    transparent_scale: 0.75
    strength: 1.8
    fade_power: 1.5
  taa:
    enabled: true
    blend: 0.8
    jitter_scale: 1.5
  svo:
    enabled: true
    near_mesh_radius_chunks: 9
    lod_start_radius_chunks: 12
    lod_view_distance_chunks: 30
    lod_cell_span_chunks: 6
    lod_chunk_sample_step: 3
    lod_max_cells: 2048
    lod_max_cpu_bytes: 262144
    lod_max_gpu_bytes: 131072
    lod_copy_budget_per_frame: 11
    lod_apply_budget_per_frame: 7
  svo_voxel:
    enabled: true
    near_mesh_radius_chunks: 7
    start_radius_chunks: 11
    max_radius_chunks: 48
    transition_band_chunks: 3
    levels: 5
    page_size_voxels: 64
    min_leaf_voxels: 4
    build_budget_pages_per_frame: 2
    apply_budget_pages_per_frame: 3
    upload_budget_pages_per_frame: 4
    max_resident_pages: 900
    max_cpu_bytes: 123456
    max_gpu_bytes: 654321
  profiling:
    enabled: true
)";

    ConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(yaml));
    WorldRenderConfig config = provider.loadRenderConfig();

    CHECK_NEAR(config.sunDirection.x, 0.2f, 0.0001f);
    CHECK_NEAR(config.sunDirection.y, 0.8f, 0.0001f);
    CHECK_NEAR(config.sunDirection.z, 0.1f, 0.0001f);
    CHECK_NEAR(config.transparentAlpha, 0.4f, 0.0001f);
    CHECK_NEAR(config.renderDistance, 300.0f, 0.0001f);
    CHECK(config.shadow.enabled);
    CHECK_EQ(config.shadow.cascades, 2);
    CHECK_EQ(config.shadow.mapSize, 512);
    CHECK_NEAR(config.shadow.maxDistance, 150.0f, 0.0001f);
    CHECK_NEAR(config.shadow.splitLambda, 0.6f, 0.0001f);
    CHECK_NEAR(config.shadow.bias, 0.001f, 0.0001f);
    CHECK_NEAR(config.shadow.normalBias, 0.01f, 0.0001f);
    CHECK_EQ(config.shadow.pcfRadius, 2);
    CHECK_EQ(config.shadow.pcfRadiusNear, 1);
    CHECK_EQ(config.shadow.pcfRadiusFar, 3);
    CHECK_NEAR(config.shadow.transparentScale, 0.75f, 0.0001f);
    CHECK_NEAR(config.shadow.strength, 1.8f, 0.0001f);
    CHECK_NEAR(config.shadow.fadePower, 1.5f, 0.0001f);
    CHECK(config.taa.enabled);
    CHECK_NEAR(config.taa.blend, 0.8f, 0.0001f);
    CHECK_NEAR(config.taa.jitterScale, 1.5f, 0.0001f);
    CHECK(config.svo.enabled);
    CHECK_EQ(config.svo.nearMeshRadiusChunks, 9);
    CHECK_EQ(config.svo.lodStartRadiusChunks, 12);
    CHECK_EQ(config.svo.lodViewDistanceChunks, 30);
    CHECK_EQ(config.svo.lodCellSpanChunks, 6);
    CHECK_EQ(config.svo.lodChunkSampleStep, 3);
    CHECK_EQ(config.svo.lodMaxCells, 2048);
    CHECK_EQ(config.svo.lodMaxCpuBytes, 262144);
    CHECK_EQ(config.svo.lodMaxGpuBytes, 131072);
    CHECK_EQ(config.svo.lodCopyBudgetPerFrame, 11);
    CHECK_EQ(config.svo.lodApplyBudgetPerFrame, 7);
    CHECK(config.svoVoxel.enabled);
    CHECK_EQ(config.svoVoxel.nearMeshRadiusChunks, 7);
    CHECK_EQ(config.svoVoxel.startRadiusChunks, 11);
    CHECK_EQ(config.svoVoxel.maxRadiusChunks, 48);
    CHECK_EQ(config.svoVoxel.transitionBandChunks, 3);
    CHECK_EQ(config.svoVoxel.levels, 5);
    CHECK_EQ(config.svoVoxel.pageSizeVoxels, 64);
    CHECK_EQ(config.svoVoxel.minLeafVoxels, 4);
    CHECK_EQ(config.svoVoxel.buildBudgetPagesPerFrame, 2);
    CHECK_EQ(config.svoVoxel.applyBudgetPagesPerFrame, 3);
    CHECK_EQ(config.svoVoxel.uploadBudgetPagesPerFrame, 4);
    CHECK_EQ(config.svoVoxel.maxResidentPages, 900);
    CHECK_EQ(config.svoVoxel.maxCpuBytes, static_cast<int64_t>(123456));
    CHECK_EQ(config.svoVoxel.maxGpuBytes, static_cast<int64_t>(654321));
    CHECK(config.profilingEnabled);
}

TEST_CASE(RenderConfig_SvoClampsInvalidValues) {
    const std::string yaml = R"(
render:
  svo:
    enabled: true
    near_mesh_radius_chunks: -2
    lod_start_radius_chunks: -4
    lod_view_distance_chunks: -8
    lod_cell_span_chunks: 0
    lod_chunk_sample_step: 999
    lod_max_cells: -8
    lod_max_cpu_bytes: -9
    lod_max_gpu_bytes: -10
    lod_copy_budget_per_frame: -3
    lod_apply_budget_per_frame: -6
)";

    ConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(yaml));
    WorldRenderConfig config = provider.loadRenderConfig();

    CHECK(config.svo.enabled);
    CHECK_EQ(config.svo.nearMeshRadiusChunks, 0);
    CHECK_EQ(config.svo.lodStartRadiusChunks, 0);
    CHECK_EQ(config.svo.lodViewDistanceChunks, 0);
    CHECK_EQ(config.svo.lodCellSpanChunks, 1);
    CHECK_EQ(config.svo.lodChunkSampleStep, Chunk::SIZE);
    CHECK_EQ(config.svo.lodMaxCells, 0);
    CHECK_EQ(config.svo.lodMaxCpuBytes, 0);
    CHECK_EQ(config.svo.lodMaxGpuBytes, 0);
    CHECK_EQ(config.svo.lodCopyBudgetPerFrame, 0);
    CHECK_EQ(config.svo.lodApplyBudgetPerFrame, 0);
}

TEST_CASE(RenderConfig_SvoVoxelClampsInvalidValues) {
    const std::string yaml = R"(
render:
  svo_voxel:
    enabled: true
    near_mesh_radius_chunks: -1
    start_radius_chunks: -2
    max_radius_chunks: -3
    transition_band_chunks: -4
    levels: 0
    page_size_voxels: 9
    min_leaf_voxels: 7
    build_budget_pages_per_frame: -1
    apply_budget_pages_per_frame: -2
    upload_budget_pages_per_frame: -3
    max_resident_pages: -4
    max_cpu_bytes: -5
    max_gpu_bytes: -6
)";

    ConfigProvider provider;
    provider.addSource(std::make_unique<StringConfigSource>(yaml));
    WorldRenderConfig config = provider.loadRenderConfig();

    CHECK(config.svoVoxel.enabled);
    CHECK_EQ(config.svoVoxel.nearMeshRadiusChunks, 0);
    CHECK_EQ(config.svoVoxel.startRadiusChunks, 0);
    CHECK_EQ(config.svoVoxel.maxRadiusChunks, 0);
    CHECK_EQ(config.svoVoxel.transitionBandChunks, 0);
    CHECK_EQ(config.svoVoxel.levels, 1);
    CHECK_EQ(config.svoVoxel.pageSizeVoxels, 16);
    CHECK_EQ(config.svoVoxel.minLeafVoxels, 8);
    CHECK_EQ(config.svoVoxel.buildBudgetPagesPerFrame, 0);
    CHECK_EQ(config.svoVoxel.applyBudgetPagesPerFrame, 0);
    CHECK_EQ(config.svoVoxel.uploadBudgetPagesPerFrame, 0);
    CHECK_EQ(config.svoVoxel.maxResidentPages, 0);
    CHECK_EQ(config.svoVoxel.maxCpuBytes, 0);
    CHECK_EQ(config.svoVoxel.maxGpuBytes, 0);
}
