# SVO LOD Benchmark Comparison Sheet

This sheet tracks chunk-only versus hybrid SVO LOD performance using the same
world seed and camera path.

## Method

1. Use the same build, hardware, and config except `render.svo.enabled`.
2. Use the same save/seed and the same camera traversal.
3. Capture profiler values after warm-up (for example 30s).
4. Record:
   - frame time average and p95
   - chunk streaming frame cost (`Streaming/*`)
   - SVO stage costs (scan/copy/apply/upload) when enabled
   - CPU/GPU memory deltas

## Runs

| Run ID | Date | Build | Seed/World | View Distance | Mode | Avg Frame (ms) | P95 Frame (ms) | Avg Streaming (ms) | Peak RSS (MiB) | Notes |
|---|---|---|---|---:|---|---:|---:|---:|---:|---|
| TBD-1 | TBD | `main` | `TBD` | TBD | Chunk-only (`svo.enabled=false`) | TBD | TBD | TBD | TBD | Baseline |
| TBD-1 | TBD | `main` | `TBD` | TBD | Hybrid (`svo.enabled=true`) | TBD | TBD | TBD | TBD | Compare vs baseline |

## Interpretation Guide

- Hybrid mode is considered a win when far-field coverage improves without
  increasing p95 frame spikes in streaming-heavy movement.
- Regressions should be investigated by comparing `Streaming/*` profiler scopes
  against SVO stage costs and memory budget settings.
