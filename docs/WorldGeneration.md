# World Generation and Chunk Management Design

This document proposes a robust world generation and chunk management system
for Rigel, grounded in established voxel engine techniques and the guidance
captured in `~/artifact.md`. The focus is realistic, climate-driven terrain.

---

## 1. Goals

- Deterministic, seed-based world generation across infinite space.
- Global-scale climate with natural transitions and elevation effects.
- Stable performance via streaming, caching, and background jobs.
- Extensible pipeline for caves, structures, and future simulation.
- Clear separation between generation, storage, meshing, and rendering.

---

## 2. Design Principles

- **World-space determinism**: all noise uses global coordinates; no seams.
- **Chunk-local isolation**: each chunk is generated independently from its
  world-space inputs and seed, no cross-chunk mutable state.
- **Thread safety**: generation and meshing are CPU-only and run off-thread;
  GPU uploads only on the main thread.
- **Data-driven tuning**: climate and biome parameters live in a configuration
  file or manifest for iteration.

---

## 3. Chunk Lifecycle and Streaming

Chunk states:

```
Missing -> Requested -> Loading -> ReadyData -> Meshing -> ReadyMesh
                     \-> Generating -> ReadyData
```

- **Requested**: enqueued by streamer; priority by distance and view direction.
- **Loading**: disk read from storage layer.
- **Generating**: seed-based generator fills block data.
- **ReadyData**: chunk exists, needs mesh.
- **Meshing**: CPU mesh build.
- **ReadyMesh**: mesh uploaded, renderable.

Streaming uses hysteresis: load within distance N, unload beyond N+H.

---

## 4. Core Components

### 4.1 ChunkStreamer

Responsible for:
- Computing desired chunk set from camera position.
- Enqueuing load/generate jobs.
- Scheduling meshing jobs.
- Evicting chunks beyond memory budget.

### 4.2 ChunkCache

- LRU cache over `ChunkCoord`.
- Configurable max chunk count.
- Eviction hook that serializes to storage.

### 4.3 ChunkStorage

```cpp
class IChunkStorage {
public:
    virtual ~IChunkStorage() = default;
    virtual bool has(ChunkCoord coord) const = 0;
    virtual std::vector<uint8_t> load(ChunkCoord coord) = 0;
    virtual void save(ChunkCoord coord, const Chunk& chunk) = 0;
};
```

Implementations:
- `DiskChunkStorage` using region-style files, LZ4 compression.
- `MemoryChunkStorage` for testing.

---

## 5. Climate-Driven Generation (Global Scale)

### 5.1 Noise Toolkit

Use the following for realism and variety:
- **Simplex/OpenSimplex** for base noise (fast, no grid artifacts).
- **fBm** for multi-octave detail.
- **Domain warping** for organic landforms.
- **3D density noise** for caves and overhangs.

fBm parameters (typical):
- lacunarity: 2.0
- persistence: 0.5
- octaves: 6-8

### 5.2 Climate Fields (Global First, Local Refine)

Compute climate in two layers:

1) **Global layer** (very low frequency, continent scale)
2) **Local layer** (higher frequency, regional variation)

Global layer drives the "big picture" climate; local layer adds detail without
breaking large-scale coherence.

- **Temperature**: base noise + latitude + elevation lapse.
- **Humidity**: base noise + distance to ocean + orographic effect.
- **Continentalness**: distance from oceans; defines coasts and inland.
- **Erosion**: controls roughness, cliffs, river likelihood.
- **Weirdness**: adds biome variety (plateaus, peaks).

All five parameters are used to pick and blend biomes.

### 5.3 Global Climate Model

Global climate should be coherent over hundreds or thousands of chunks:

- **Latitude**: map world Z to a latitude curve (equator → poles).
- **Prevailing winds**: configurable wind direction used for rainfall modeling.
- **Ocean influence**: continentalness computed from low-frequency noise or
  derived ocean masks.
- **Orographic lift**: humidity increases on windward slopes.
- **Temperature lapse**: cooling with altitude.

The model is tunable by global-scale curves and multipliers.

### 5.4 Biome Selection and Blending

Biomes are defined by target values across the 5D climate space.
For each position:

1. Compute distance to each biome target.
2. Convert to weight via inverse distance and exponential falloff.
3. Normalize weights and blend biome parameters (surface blocks, height offsets).

This yields smooth transitions instead of hard borders.

### 5.5 Terrain Height and Density

Use a hybrid heightmap + density field:

```
baseHeight = heightNoise(x, z)
density3D = densityNoise(x, y, z)
finalDensity = density3D + (baseHeight - y) * gradientStrength
solid if finalDensity > 0
```

This enables cliffs, overhangs, and floating features while keeping
large-scale heightmap structure.

### 5.6 Cave Generation

Two layers:
- **Threshold caves**: 3D noise, carve above threshold (cheese caves).
- **Worms**: agent-based tunnel carving for long, organic caves.

Optional: noise types for spaghetti vs noodle tunnels.

---

## 6. Generation Pipeline

Chunk generation operates on a fixed, data-driven pipeline. Each stage is
configurable but the stage ordering is fixed for deterministic behavior.

### 6.1 Pipeline Stages (Fixed Order)

1. **Inputs and Coordinate Prep**
   - Compute world-space position for each voxel.
   - Compute chunk-local position and masks for fast access.

2. **Global Climate Fields**
   - Sample global-scale climate signals (temperature, humidity,
     continentalness).

3. **Local Climate Refinement**
   - Add local-scale noise variation without breaking global continuity.

4. **Biome Resolution**
   - Evaluate biome weights from climate space.
   - Produce blended biome parameters.

5. **Terrain Height and Base Density**
   - Compute base height (continentalness curves).
   - Compute base density from height and 3D noise.
   - When configured, `density_graph.outputs.base_density` supplies the density.

6. **Caves and Density Carving**
   - Apply threshold caves and worm tunnels (configurable).
   - When configured, `density_graph.outputs.cave_density` supplies the carve signal.

7. **Surface Rules**
   - Apply biome surface materials (topsoil, subsoil, rock strata).

8. **Structures**
   - Place structures deterministically (trees, boulders).

9. **Post-Process**
   - Apply optional passes (ore distribution, future lighting).

The pipeline produces a `ChunkBuffer` with final block states.

### 6.2 Data-Driven Stage Configuration

Each stage has a configuration block. The engine uses a strict schema:

```yaml
generation:
  pipeline:
    - stage: climate_global
      params: climate_global
    - stage: climate_local
      params: climate_local
    - stage: biome_resolve
      params: biome_resolve
    - stage: terrain_density
      params: terrain_density
    - stage: caves
      params: caves
    - stage: surface_rules
      params: surface_rules
    - stage: structures
      params: structures
    - stage: post_process
      params: post_process
```

Pipeline ordering is fixed in code; the config only controls parameters and
enabled flags. If a pipeline list is provided, it is validated against the
fixed order and cannot reorder stages.

### 6.3 Declarative Generator Construction

Generators are created declaratively by a registry of stage factories:

- Each stage type registers a factory by name
  (e.g., `climate_global`, `caves`, `surface_rules`).
- The generator builder instantiates stages from the config snapshot.
- Missing stages fall back to defaults; disabled stages are skipped.

This keeps the generator configurable without coupling it to runtime systems.

---

## 7. Realistic Climate Rules

### 7.1 Temperature

```
temp = baseTempNoise(x, z)
temp -= elevation * lapseRate
temp += latitudeInfluence
```

- `lapseRate` cools with altitude.
- Latitude introduces poles/equator.

### 7.2 Humidity

```
humidity = baseHumidityNoise(x, z)
humidity -= distanceToOcean * coastalFalloff
humidity += orographicLift(elevation, windDir)
```

Orographic lift increases rainfall on windward slopes.

### 7.3 Biome Examples

| Biome | Temp | Humidity | Continentalness | Erosion | Notes |
|-------|------|----------|-----------------|---------|-------|
| Tundra | low | low | high | low | Snow, ice |
| Plains | mid | mid | mid | mid | Grass, gentle hills |
| Desert | high | low | mid | high | Sand, dunes |
| Jungle | high | high | low-mid | low | Dense foliage |
| Mountains | low-mid | mid | high | low | Steep slopes |

Blend these for realistic transitions.

---

## 8. Configuration and Tuning

The engine should be tunable without code changes. Use a layered config
provider that can merge multiple sources (embedded defaults, user overrides,
mod packs) without restricting runtime behavior.

Config defines:

- **World bounds and versioning**:
  - `world.min_y`, `world.max_y`, `world.sea_level`, `world.lava_level`
  - `world.version` for regeneration when the pipeline changes
- **World seed** and world size scaling (meters per block, km per chunk).
- **Global climate curves**:
  - latitude → temperature multiplier
  - continentalness → base height
- **Noise stack parameters**:
  - octave count, lacunarity, persistence
  - domain warp strength, frequency ranges
- **Biome table**:
  - target climate values
  - blending exponent
  - surface block layers
  - structure lists and probabilities
- **Density graph**:
  - named nodes and outputs (`base_density`, `cave_density`)
  - node types like noise, add/mul, clamp, spline, and climate lookups
- **Cave tuning**:
  - threshold values
  - worm counts, step sizes, radius ranges
- **Overlays**:
  - optional configs merged when `flags` toggle on

Configuration is loaded at startup and may be hot-reloaded when supported by
the active config source.

---

## 9. Chunk Size and Storage

Keep 32x32x32 chunk size for now. The design can evolve to:
- **Palette compression** in-memory for sparse chunk data.
- **RLE + LZ4** for disk storage.

Serialization:
- Region files (32x32 chunk groups).
- Chunk data: header + compressed block data.

---

## 10. Meshing and Rendering Implications

Planned optimizations (from artifact guidance):
- **Binary greedy meshing** for high throughput.
- **AO**: 3-neighbor sampling; flip quad diagonal when AO gradients differ.
- **Face culling** and **frustum culling** before draw.

Mesh build stays off-thread; GPU uploads are main-thread only.

---

## 11. Task System

Work queues:

- `LoadQueue`: disk IO.
- `GenQueue`: climate + terrain generation.
- `MeshQueue`: MeshBuilder work.
- `MainThreadQueue`: GPU uploads and chunk state updates.

Use priority queues based on camera distance and direction.

---

## 12. Integration with Current Code

### New Classes

- `WorldGenerator`: climate-driven pipeline.
- `ChunkStreamer`: chunk lifecycle management.
- `ChunkCache`: LRU + hysteresis.
- `IChunkStorage`: persistence interface.

### World API Changes

`Voxel::World` gains:
- `setGenerator(...)`
- `setStorage(...)`
- `updateStreaming(cameraPos)`
- `updateMeshes()` called from streamer only when data ready.

---

## 13. Milestone Plan

### A. Streaming Skeleton
- `ChunkStreamer`, `ChunkCache`, basic load/unload radius.
- In-memory only (no disk IO).

### B. Climate Generator
- Seeded climate fields.
- Height + density fill with biome surface rules.

### C. Async Meshing
- Background mesh build, main-thread upload.

### D. Persistence
- Region file storage + LZ4 compression.
- Save on eviction.

### E. Biomes + Structures
- Biome blending and structure placement.

---

## 14. Open Risks

- Tuning climate parameters for realism without heavy simulation.
- Balancing generation cost with chunk throughput.
- Avoiding structure duplication across chunk boundaries.
- Format versioning for future block palette changes.

---

## 15. Next Implementation Steps

1. Add `ChunkStreamer` + `ChunkCache` with memory budget and hysteresis.
2. Add `WorldGenerator` scaffolding with global climate fields + config load.
3. Implement climate-to-biome mapping with blended transitions.
4. Add async mesh queue and main-thread upload queue.
5. Integrate disk storage after stability.
