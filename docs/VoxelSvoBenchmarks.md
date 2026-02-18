# Voxel SVO Benchmark Comparison Sheet

This sheet tracks chunk-only versus voxel-SVO far rendering performance using
the same world seed and camera path.

## Method

1. Use the same build, hardware, and config except `render.svo_voxel.enabled`.
2. Use the same save/seed and the same camera traversal.
3. Capture profiler values after warm-up (for example 30s).
4. Record:
   - frame time average and p95
   - chunk streaming frame cost (`Streaming/*`)
   - voxel-SVO stage costs (sample/build/mesh/upload)
   - CPU/GPU memory deltas

## Runs

| Run ID | Date | Build | Seed/World | View Distance | Mode | Avg Frame (ms) | P95 Frame (ms) | Avg Streaming (ms) | Peak RSS (MiB) | Notes |
|---|---|---|---|---:|---|---:|---:|---:|---:|---|
| TBD-1 | TBD | `main` | `TBD` | TBD | Chunk-only (`svo_voxel.enabled=false`) | TBD | TBD | TBD | TBD | Baseline |
| TBD-2 | TBD | `main` | `TBD` | TBD | Hybrid near+voxel-SVO (`svo_voxel.enabled=true`) | TBD | TBD | TBD | TBD | Compare vs baseline |

## Interpretation Guide

- Voxel-SVO mode is considered a win when far-field coverage improves without
  increasing p95 frame spikes in streaming-heavy movement.
- Regressions should be investigated by comparing `Streaming/*` profiler scopes
  against voxel-SVO stage costs and memory budget settings.
